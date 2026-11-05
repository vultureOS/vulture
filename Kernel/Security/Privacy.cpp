/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/privacy.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Security/Privacy.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>
#include <LibBase/Vector.h>

namespace Kernel::Privacy {

namespace {

// Longer than anyone takes to read a question, short of having walked away.
constexpr u64 answer_timeout_milliseconds = 300'000;

struct Question {
    vulture_privacy_request request {};
    bool delivered { false };
    // 0 not yet; 1 allowed; 2 not.
    Atomic<u8> answer { 0 };
};

SpinLock s_lock;
Vector<Question*> s_questions;
u32 s_next_id { 1 };
Atomic<u32> s_readers { 0 };
WaitQueue s_answered;
WaitQueue s_asked;

class PrivacyDevice final : public Device {
public:
    PrivacyDevice()
        : Device(InodeType::CharacterDevice, 0600, device_number(14, 5))
    {
    }

    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        if (count < sizeof(vulture_privacy_request))
            return Error::from_errno(EINVAL);
        for (;;) {
            vulture_privacy_request copy;
            bool found = false;
            {
                SpinLockGuard guard(s_lock);
                for (auto* question : s_questions) {
                    if (!question->delivered) {
                        question->delivered = true;
                        copy = question->request;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                TRY(buffer.write(0, { reinterpret_cast<u8 const*>(&copy), sizeof(copy) }));
                return sizeof(copy);
            }
            if (!s_asked.wait_until_interruptible([] { return has_undelivered.load() != 0; }))
                return Error::from_errno(EINTR);
            has_undelivered.store(0);
        }
    }

    ErrorOr<size_t> write(u64, size_t count, IOBuffer const& buffer) override
    {
        if (count != sizeof(vulture_privacy_answer))
            return Error::from_errno(EINVAL);
        vulture_privacy_answer answer;
        TRY(buffer.read(0, { reinterpret_cast<u8*>(&answer), sizeof(answer) }));
        {
            SpinLockGuard guard(s_lock);
            for (auto* question : s_questions) {
                if (question->request.id == answer.id) {
                    question->answer.store(answer.allowed ? 1 : 2);
                    break;
                }
            }
        }
        s_answered.wake_all();
        return count;
    }

    [[nodiscard]] bool can_read() const override
    {
        SpinLockGuard guard(s_lock);
        for (auto const* question : s_questions) {
            if (!question->delivered)
                return true;
        }
        return false;
    }
    [[nodiscard]] bool can_write() const override { return true; }

    void description_opened() override { s_readers.fetch_add(1); }
    void description_closed() override { s_readers.fetch_sub(1); }

    static Atomic<u8> has_undelivered;
};

Atomic<u8> PrivacyDevice::has_undelivered { 0 };

// Which service a path is in, if any: the third part of /Users/NAME/Documents/...
u32 service_of(StringView path)
{
    constexpr StringView users = "/Users/"_sv;
    if (!path.starts_with(users))
        return 0;
    auto const rest = path.substring_view(users.length());
    auto const slash = rest.find('/');
    if (!slash.has_value())
        return 0;
    auto folder = rest.substring_view(slash.value() + 1);
    if (auto const next = folder.find('/'); next.has_value())
        folder = folder.substring_view(0, next.value());
    if (folder == "Desktop"_sv)
        return VULTURE_PRIVACY_DESKTOP;
    if (folder == "Documents"_sv)
        return VULTURE_PRIVACY_DOCUMENTS;
    if (folder == "Downloads"_sv)
        return VULTURE_PRIVACY_DOWNLOADS;
    return 0;
}

// What came of asking. Only a real answer stays with the process: nobody being there to ask this
// time doesn't mean no for as long as it runs.
enum class Outcome {
    Allowed,
    Denied,
    NoAnswer,
    Interrupted,
};

Outcome ask(Process& process, u32 service)
{
    if (s_readers.load() == 0)
        return Outcome::NoAnswer;
    auto* question = new (Base::nothrow) Question;
    if (!question)
        return Outcome::NoAnswer;
    auto const identity = process.responsible_application();
    question->request.service = service;
    question->request.pid = process.pid();
    question->request.uid = process.credentials().uid;
    __builtin_memcpy(question->request.application, identity.characters_without_null_termination(), min<size_t>(identity.length(), sizeof(question->request.application) - 1));
    {
        SpinLockGuard guard(s_lock);
        question->request.id = s_next_id++;
        if (s_questions.try_append(question).is_error()) {
            delete question;
            return Outcome::NoAnswer;
        }
    }
    PrivacyDevice::has_undelivered.store(1);
    s_asked.wake_all();
    notify_pollers();
    auto const result = s_answered.wait_until_interruptible([question] { return question->answer.load() != 0; },
        Time::now() + Time::ticks_from_milliseconds(answer_timeout_milliseconds));
    u8 const answer = question->answer.load();
    {
        SpinLockGuard guard(s_lock);
        s_questions.remove_all_matching([question](auto* other) { return other == question; });
    }
    delete question;
    if (answer == 1)
        return Outcome::Allowed;
    if (answer == 2)
        return Outcome::Denied;
    return result == WaitQueue::WaitResult::Interrupted ? Outcome::Interrupted : Outcome::NoAnswer;
}

}

void initialize()
{
    auto* device = new (Base::nothrow) PrivacyDevice;
    if (!device)
        return;
    if (auto added = DevFS::the().add_device("privacy"_sv, NonnullRefPtr<Inode>(*static_cast<Inode*>(device))); added.is_error())
        dmesgln("Privacy: /dev/privacy: {}", added.error());
}

ErrorOr<void> check(StringView path)
{
    u32 const service = service_of(path);
    if (service == 0)
        return {};
    return check_service(service);
}

bool has_been_allowed(int pid, u32 service)
{
    auto process = Process::from_pid(pid);
    if (!process)
        return false;
    // What came with the system isn't asked about.
    if (process->responsible_application().is_empty())
        return true;
    return process->privacy_allowed() & static_cast<u8>(1u << service);
}

ErrorOr<void> check_service(u32 service)
{
    auto* process = Process::current();
    if (!process || process->responsible_application().is_empty())
        return {};
    u8 const bit = static_cast<u8>(1u << service);
    if (process->privacy_allowed() & bit)
        return {};
    if (process->privacy_denied() & bit)
        return Error::from_errno(EPERM);
    auto const outcome = ask(*process, service);
    if (outcome == Outcome::Interrupted)
        return Error::from_errno(EINTR);
    // It keeps an answer until it quits: changing their mind in Settings applies from its next launch,
    // and an application that has been told no isn't to go on asking.
    if (outcome == Outcome::Allowed || outcome == Outcome::Denied)
        process->remember_privacy_answer(bit, outcome == Outcome::Allowed);
    if (outcome != Outcome::Allowed) {
        dmesgln("Privacy: {}[{}], from {}, denied service {}", process->name(), process->pid(), process->responsible_application(), service);
        return Error::from_errno(EPERM);
    }
    return {};
}

}
