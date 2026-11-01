/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Format.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// ipcc INPUT.ipc OUTPUT.h
//
// Turns endpoint descriptions into C++ for LibIPC: for each endpoint, the
// message numbers, a proxy whose methods send requests (and wait for the
// answers), and a stub a server implements, whose handle() decodes a
// message, calls the method and sends its answer.
//
//     // Comments like this.
//     endpoint Bootstrap
//     {
//         look_up(String name) => (i32 error, Port service)
//         log(u8 level, String message) =|
//     }
//
// "=>" names what the answer holds; "=|" means there is no answer (the
// proxy does not wait). Types: bool, u8-u64, i8-i64, String, ByteBuffer,
// Port (a port right, or any descriptor), and Vector<T> of any but Port.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
ssize_t write(int fd, void const* buffer, size_t count);
void* malloc(size_t);
void free(void*);
}

namespace {

class StandardError final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(2, text.characters_without_null_termination(), text.length()); }
};

template<typename... Args>
int fail(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    StandardError error;
    error.write("ipcc: "_sv);
    format_to(error, format, args...);
    error.write("\n"_sv);
    return 1;
}

struct Parameter {
    String type;
    String name;
};

struct Method {
    String name;
    Vector<Parameter> parameters;
    Vector<Parameter> results;
    bool asynchronous { false };
};

struct Endpoint {
    String name;
    Vector<Method> methods;
};

class Lexer {
public:
    explicit Lexer(StringView text)
        : m_text(text)
    {
    }

    // The next token: a name, a type with its <...>, or punctuation; empty
    // at the end.
    StringView next()
    {
        skip();
        if (m_position >= m_text.length())
            return {};
        size_t start = m_position;
        char c = m_text[m_position];
        if (is_name_character(c)) {
            while (m_position < m_text.length() && is_name_character(m_text[m_position]))
                ++m_position;
            // A type such as Vector<String>: one token.
            if (m_position < m_text.length() && m_text[m_position] == '<') {
                int depth = 0;
                while (m_position < m_text.length()) {
                    char d = m_text[m_position++];
                    if (d == '<')
                        ++depth;
                    else if (d == '>' && --depth == 0)
                        break;
                }
            }
            return m_text.substring_view(start, m_position - start);
        }
        if ((c == '=' && m_position + 1 < m_text.length() && (m_text[m_position + 1] == '>' || m_text[m_position + 1] == '|'))) {
            m_position += 2;
            return m_text.substring_view(start, 2);
        }
        ++m_position;
        return m_text.substring_view(start, 1);
    }

    StringView peek()
    {
        size_t saved = m_position;
        auto token = next();
        m_position = saved;
        return token;
    }

    [[nodiscard]] size_t line() const
    {
        size_t line = 1;
        for (size_t i = 0; i < m_position && i < m_text.length(); ++i)
            line += m_text[i] == '\n';
        return line;
    }

private:
    static bool is_name_character(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    void skip()
    {
        while (m_position < m_text.length()) {
            char c = m_text[m_position];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_position;
            } else if (c == '/' && m_position + 1 < m_text.length() && m_text[m_position + 1] == '/') {
                while (m_position < m_text.length() && m_text[m_position] != '\n')
                    ++m_position;
            } else {
                return;
            }
        }
    }

    StringView m_text;
    size_t m_position { 0 };
};

bool is_known_type(StringView type)
{
    char const* const simple[] = { "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "String", "ByteBuffer", "Port" };
    for (auto const* name : simple) {
        if (type == StringView(name))
            return true;
    }
    if (type.starts_with("Vector<"_sv) && type.ends_with('>')) {
        auto element = type.substring_view(7, type.length() - 8);
        return element != "Port"_sv && is_known_type(element);
    }
    return false;
}

// How a value is held: in a stub's arguments, answers and structs.
String storage_type(StringView type)
{
    StringBuilder builder;
    if (type == "String"_sv)
        builder.append("String"_sv);
    else if (type == "ByteBuffer"_sv)
        builder.append("Vector<u8>"_sv);
    else if (type == "Port"_sv)
        builder.append("IPC::Port"_sv);
    else if (type.starts_with("Vector<"_sv)) {
        builder.append("Vector<"_sv);
        builder.append(storage_type(type.substring_view(7, type.length() - 8)).view());
        builder.append('>');
    } else
        builder.append(type);
    return MUST(builder.to_string());
}

// How a proxy takes it.
String parameter_type(StringView type)
{
    StringBuilder builder;
    if (type == "String"_sv)
        builder.append("StringView"_sv);
    else if (type == "ByteBuffer"_sv)
        builder.append("ReadonlyBytes"_sv);
    else if (type == "Port"_sv)
        builder.append("IPC::Port"_sv);
    else if (type.starts_with("Vector<"_sv)) {
        builder.append(storage_type(type).view());
        builder.append(" const&"_sv);
    } else
        builder.append(type);
    return MUST(builder.to_string());
}

String pascal_case(StringView name)
{
    StringBuilder builder;
    bool upper = true;
    for (char c : name) {
        if (c == '_') {
            upper = true;
            continue;
        }
        builder.append(upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        upper = false;
    }
    return MUST(builder.to_string());
}

u32 magic_of(StringView name)
{
    u32 hash = 2166136261u;
    for (char c : name) {
        hash ^= static_cast<u8>(c);
        hash *= 16777619u;
    }
    return hash;
}

bool parse_parameters(Lexer& lexer, Vector<Parameter>& parameters, StringView& error)
{
    if (lexer.next() != "("_sv) {
        error = "expected '('"_sv;
        return false;
    }
    if (lexer.peek() == ")"_sv) {
        lexer.next();
        return true;
    }
    for (;;) {
        auto type = lexer.next();
        auto name = lexer.next();
        if (!is_known_type(type)) {
            error = "unknown type (bool, u8-u64, i8-i64, String, ByteBuffer, Port, Vector<...>)"_sv;
            return false;
        }
        if (name.is_empty()) {
            error = "expected a parameter's name"_sv;
            return false;
        }
        parameters.append({ MUST(String::from_view(type)), MUST(String::from_view(name)) });
        auto separator = lexer.next();
        if (separator == ")"_sv)
            return true;
        if (separator != ","_sv) {
            error = "expected ',' or ')'"_sv;
            return false;
        }
    }
}

bool parse(StringView text, Vector<Endpoint>& endpoints, size_t& error_line, StringView& error)
{
    Lexer lexer(text);
    for (;;) {
        auto keyword = lexer.next();
        if (keyword.is_empty())
            return true;
        if (keyword != "endpoint"_sv) {
            error = "expected 'endpoint'"_sv;
            error_line = lexer.line();
            return false;
        }
        Endpoint endpoint;
        endpoint.name = MUST(String::from_view(lexer.next()));
        if (lexer.next() != "{"_sv) {
            error = "expected '{'"_sv;
            error_line = lexer.line();
            return false;
        }
        while (lexer.peek() != "}"_sv) {
            Method method;
            method.name = MUST(String::from_view(lexer.next()));
            if (method.name.is_empty()) {
                error = "unexpected end inside the endpoint"_sv;
                error_line = lexer.line();
                return false;
            }
            if (!parse_parameters(lexer, method.parameters, error)) {
                error_line = lexer.line();
                return false;
            }
            auto arrow = lexer.next();
            if (arrow == "=|"_sv) {
                method.asynchronous = true;
            } else if (arrow == "=>"_sv) {
                if (!parse_parameters(lexer, method.results, error)) {
                    error_line = lexer.line();
                    return false;
                }
            } else {
                error = "expected '=>' or '=|'"_sv;
                error_line = lexer.line();
                return false;
            }
            endpoint.methods.append(move(method));
        }
        lexer.next();
        endpoints.append(move(endpoint));
    }
}

void generate(StringBuilder& out, Endpoint const& endpoint)
{
    auto const& name = endpoint.name;
    out.appendff("// The {} endpoint.\nstruct {}Endpoint {{\n", name, name);
    out.appendff("    static constexpr u32 magic = {:#x};\n", magic_of(name.view()));
    out.append("    enum class Method : u32 {\n"_sv);
    for (size_t i = 0; i < endpoint.methods.size(); ++i)
        out.appendff("        {} = {},\n", pascal_case(endpoint.methods[i].name.view()), i + 1);
    out.append("    };\n"_sv);
    for (auto const& method : endpoint.methods) {
        if (method.results.size() < 2)
            continue;
        out.appendff("    struct {}Answer {{\n", pascal_case(method.name.view()));
        for (auto const& result : method.results)
            out.appendff("        {} {} {{}};\n", storage_type(result.type.view()), result.name);
        out.append("    };\n"_sv);
    }
    out.append("};\n\n"_sv);

    auto answer_type = [&](Method const& method) -> String {
        if (method.results.is_empty())
            return MUST(String::from_view("void"_sv));
        if (method.results.size() == 1)
            return storage_type(method.results[0].type.view());
        StringBuilder builder;
        builder.appendff("{}Endpoint::{}Answer", name, pascal_case(method.name.view()));
        return MUST(builder.to_string());
    };

    // The proxy.
    out.appendff("// Talks to a {} server through a send right.\nclass {}Proxy {{\npublic:\n", name, name);
    out.appendff("    {}Proxy() = default;\n    explicit {}Proxy(IPC::Port port)\n        : m_port(move(port))\n    {{\n    }}\n\n", name, name);
    out.append("    [[nodiscard]] IPC::Port const& port() const { return m_port; }\n"_sv);
    for (size_t index = 0; index < endpoint.methods.size(); ++index) {
        auto const& method = endpoint.methods[index];
        auto answer = answer_type(method);
        out.appendff("\n    ErrorOr<{}> {}(", method.asynchronous ? "void"_sv : answer.view(), method.name);
        for (size_t i = 0; i < method.parameters.size(); ++i)
            out.appendff("{}{} {}", i ? ", "_sv : ""_sv, parameter_type(method.parameters[i].type.view()), method.parameters[i].name);
        out.append(")\n    {\n"_sv);
        out.appendff("        IPC::Encoder ipc_encoder({}Endpoint::magic, {});\n", name, index + 1);
        for (auto const& parameter : method.parameters) {
            if (parameter.type == "Port"_sv)
                out.appendff("        ipc_encoder.encode(move({}));\n", parameter.name);
            else
                out.appendff("        ipc_encoder.encode({});\n", parameter.name);
        }
        if (method.asynchronous) {
            out.append("        return IPC::send(m_port, ipc_encoder);\n    }\n"_sv);
            continue;
        }
        out.append("        auto ipc_reply = TRY(IPC::call(m_port, ipc_encoder));\n"_sv);
        out.appendff("        if (ipc_reply.id != ({} | IPC::answer_bit))\n            return Error::from_errno(EBADMSG);\n", index + 1);
        out.append("        IPC::Decoder ipc_decoder(ipc_reply);\n"_sv);
        out.appendff("        TRY(ipc_decoder.expect_magic({}Endpoint::magic));\n", name);
        if (method.results.is_empty()) {
            out.append("        return {};\n    }\n"_sv);
        } else if (method.results.size() == 1) {
            out.appendff("        {} ipc_result {{}};\n        TRY(ipc_decoder.decode(ipc_result));\n        return ipc_result;\n    }}\n", storage_type(method.results[0].type.view()));
        } else {
            out.appendff("        {} ipc_answer;\n", answer);
            for (auto const& result : method.results)
                out.appendff("        TRY(ipc_decoder.decode(ipc_answer.{}));\n", result.name);
            out.append("        return ipc_answer;\n    }\n"_sv);
        }
    }
    out.append("\nprivate:\n    IPC::Port m_port;\n};\n\n"_sv);

    // The stub.
    out.appendff("// What a {} server implements; handle() serves one message.\nclass {}Stub {{\npublic:\n    virtual ~{}Stub() = default;\n\n", name, name, name);
    for (auto const& method : endpoint.methods) {
        out.appendff("    virtual {} {}(IPC::Sender const&", method.asynchronous ? "void"_sv : answer_type(method).view(), method.name);
        for (auto const& parameter : method.parameters)
            out.appendff(", {} {}", storage_type(parameter.type.view()), parameter.name);
        out.append(") = 0;\n"_sv);
    }
    out.append("\n    // Decodes `message`, calls the method, sends the answer. EBADMSG for a\n    // message that is not one of ours.\n"_sv);
    out.append("    ErrorOr<void> handle(IPC::ReceivedMessage& ipc_message)\n    {\n        IPC::Decoder ipc_decoder(ipc_message);\n"_sv);
    out.appendff("        TRY(ipc_decoder.expect_magic({}Endpoint::magic));\n        switch (ipc_message.id) {{\n", name);
    for (size_t index = 0; index < endpoint.methods.size(); ++index) {
        auto const& method = endpoint.methods[index];
        out.appendff("        case {}: {{\n", index + 1);
        for (auto const& parameter : method.parameters)
            out.appendff("            {} {} {{}};\n            TRY(ipc_decoder.decode({}));\n", storage_type(parameter.type.view()), parameter.name, parameter.name);
        out.append("            if (!ipc_decoder.at_end())\n                return Error::from_errno(EBADMSG);\n"_sv);
        StringBuilder call;
        call.appendff("{}(ipc_message.sender", method.name);
        for (auto const& parameter : method.parameters)
            call.appendff(", move({})", parameter.name);
        call.append(')');
        if (method.asynchronous) {
            out.appendff("            {};\n            return {{}};\n        }}\n", call.view());
            continue;
        }
        if (!method.results.is_empty())
            out.appendff("            auto ipc_result = {};\n", call.view());
        else
            out.appendff("            {};\n", call.view());
        out.appendff("            IPC::Encoder ipc_answer({}Endpoint::magic, {} | IPC::answer_bit);\n", name, index + 1);
        if (method.results.size() == 1) {
            out.append(method.results[0].type == "Port"_sv ? "            ipc_answer.encode(move(ipc_result));\n"_sv : "            ipc_answer.encode(ipc_result);\n"_sv);
        } else {
            for (auto const& result : method.results) {
                if (result.type == "Port"_sv)
                    out.appendff("            ipc_answer.encode(move(ipc_result.{}));\n", result.name);
                else
                    out.appendff("            ipc_answer.encode(ipc_result.{});\n", result.name);
            }
        }
        out.append("            return IPC::answer(ipc_message, ipc_answer);\n        }\n"_sv);
    }
    out.append("        default:\n            return Error::from_errno(EBADMSG);\n        }\n    }\n};\n\n"_sv);
}

}

int main(int argc, char** argv)
{
    if (argc != 3)
        return fail("usage: ipcc INPUT.ipc OUTPUT.h");
    FILE* input = fopen(argv[1], "rb");
    if (!input)
        return fail("cannot read {}", argv[1]);
    StringBuilder text;
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), input)) > 0)
        text.append(StringView(buffer, got));
    fclose(input);

    Vector<Endpoint> endpoints;
    size_t error_line = 0;
    StringView error;
    if (!parse(text.view(), endpoints, error_line, error))
        return fail("{}:{}: {}", argv[1], error_line, error);

    StringBuilder out;
    // The file's name only: the output must not depend on where the tree is.
    StringView input_name(argv[1]);
    if (auto slash = input_name.find_last('/'); slash.has_value())
        input_name = input_name.substring_view(slash.value() + 1);
    out.appendff("// Generated by ipcc from {}. Do not edit.\n\n#pragma once\n\n", input_name);
    out.append("#include <LibIPC/Message.h>\n#include <errno.h>\n\n"_sv);
    for (auto const& endpoint : endpoints)
        generate(out, endpoint);

    FILE* output = fopen(argv[2], "wb");
    if (!output)
        return fail("cannot write {}", argv[2]);
    auto view = out.view();
    bool written = fwrite(view.characters_without_null_termination(), 1, view.length(), output) == view.length();
    if (fclose(output) != 0 || !written)
        return fail("cannot write {}", argv[2]);
    return 0;
}
