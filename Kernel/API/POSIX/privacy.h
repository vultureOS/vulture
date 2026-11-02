/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// /dev/privacy: how the kernel asks privacyd whether an application may have something. Reading gives
// the next question, whole, and writing answers it. See Kernel/Security/Privacy.h.

#define VULTURE_PRIVACY_DESKTOP 1u
#define VULTURE_PRIVACY_DOCUMENTS 2u
#define VULTURE_PRIVACY_DOWNLOADS 3u

struct vulture_privacy_request {
    unsigned int id;
    unsigned int service;
    int pid;
    unsigned int uid;
    char application[256];
};

struct vulture_privacy_answer {
    unsigned int id;
    unsigned int allowed;
};
