/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibJSON/JSON.h>
#include <LibUI/Panels.h>

// What a document is apart from what's in it: what was opened lately, the earlier versions of it, where an unsaved one is
// kept meanwhile, and which windows a program had open.

namespace Desktop {

// ---- Recent Items, as the system's menu lists them. The desktop keeps them, so that anyone can add to them.
void note_recent_document(StringView path);
void note_recent_application(StringView bundle_path);
[[nodiscard]] Vector<String> recent_documents();
[[nodiscard]] Vector<String> recent_applications();
void clear_recent_items();

// The list itself: for the desktop, which is who writes it.
void add_recent_item(StringView path, bool is_application);

// ---- Versions of a document (~/Library/Application Support/Document Revisions): File > Revert To.
struct DocumentVersion {
    String file;
    i64 time { 0 };
    String label;
};

class DocumentVersions {
public:
    explicit DocumentVersions(StringView document_path);

    // Newest first.
    [[nodiscard]] Vector<DocumentVersion> list() const;
    // Unless the newest is already this.
    ErrorOr<void> add(StringView contents, StringView label);
    // The newest with this label.
    [[nodiscard]] Optional<DocumentVersion> newest_labelled(StringView label) const;
    [[nodiscard]] ErrorOr<String> read(DocumentVersion const&) const;

private:
    void write_index(Vector<DocumentVersion> const&) const;

    String m_folder;
};

// ---- Where a document with no name yet is kept: ~/Library/Autosave Information.
[[nodiscard]] String new_autosave_path();

// A file written so that what's there is the old one or the new one, whatever happens.
ErrorOr<void> write_file_atomically(StringView path, StringView contents);

// ---- What a program had open: ~/Library/Saved Application State/IDENTIFIER.json. It's there if the program didn't get to
// quit, by crashing, being killed, or the session ending; a program that's quit normally takes it away.
class SavedState {
public:
    explicit SavedState(StringView identifier);

    [[nodiscard]] Optional<JSON::Value> load() const;
    void save(JSON::Value const&) const;
    void clear() const;

private:
    String m_path;
};

// ---- Settings > Desktop & Dock, for what's asked of documents.
[[nodiscard]] bool asks_to_keep_changes();
[[nodiscard]] bool closes_windows_when_quitting();

// ---- The Open and Save panels: in a sandbox, shown by the desktop, which is who can see the files. What's chosen can then
// be opened, and nothing else is. `then` gets the path, or nothing if it was cancelled.
void run_file_panel(UI::Window&, UI::FilePanel::Kind, StringView directory, StringView suggested_name, Function<void(StringView)> then);
// What the field at the top of a panel asks.
void use_search_in_file_panels();

}
