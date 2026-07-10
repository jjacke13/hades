---
name: email
description: Read the user's email and send email for them using the himalaya CLI. Load this before any email task; it includes install steps if himalaya is missing.
---

# Email via himalaya

You access the user's mailbox with the `himalaya` command-line program.
Use the `run_command` tool to read mail. Sending uses the `shell` tool and always asks the user for approval — that is expected; never try to avoid it.

## 1. Check it works

Run: `himalaya envelope list -o json`

- You get JSON → himalaya works, continue.
- "command not found" → install it (step 2).
- An account or config error → tell the user to run `himalaya` once in their own terminal; it starts an interactive account wizard. You cannot run the wizard for them.

## 2. Install (only if missing)

Try in order until one succeeds (covers Nix, macOS, Linux, Windows, any OS with Rust):

1. `nix profile install nixpkgs#himalaya`
2. `brew install himalaya`
3. `cargo install himalaya`
4. `scoop install himalaya`

If none works, ask the user to install it manually (https://github.com/pimalaya/himalaya), then configure an account (step 1).

## 3. List messages

`himalaya envelope list -o json`

Returns a JSON array; each item has `id`, `from`, `subject`, `date`, `flags`.
A message whose `flags` does NOT contain "Seen" is unread.
Never invent an id — only use ids that appeared in this list.

## 4. Read one message

`himalaya message read <id>` — plain-text output. Use the id from step 3.

## 5. Send or reply (user approval required)

Do it in three steps so quoting can never break:

1. With `write_file`, save the COMPLETE message to `workspace/email_draft.txt`.
   - New mail — headers, blank line, then body:
     ```
     To: someone@example.com
     Subject: The subject line

     The body text.
     ```
   - Reply — the file contains ONLY the body text (himalaya builds the reply headers).
2. Show the user the draft in your reply and say who it goes to.
3. Send with the `shell` tool:
   - New mail: `himalaya message send < workspace/email_draft.txt`
   - Reply: `himalaya message reply <id> < workspace/email_draft.txt`

## Rules

- Plain-text mail only; no attachments.
- Multiple accounts: put `-a <accountname>` right after `himalaya` in any command.
- In a scheduled task, if there is nothing important to report, reply exactly SILENT.
