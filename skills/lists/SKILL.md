---
name: lists
description: Keep multiple named todo/reference lists as files under workspace/lists/ (groceries, project backlogs, ideas). Load this when the user asks for a list OTHER than your current task plan.
---

# Multiple named lists

Your `todo` tool holds ONE list: the plan for work you are doing RIGHT NOW (it is shown to
you every turn). Everything else — groceries, a project backlog, gift ideas, films to watch —
lives as a separate file under `workspace/lists/`, one file per list. These files are NOT
shown to you automatically: read one only when its topic comes up.

## Format

One file per list: `workspace/lists/<name>.md` (short lowercase name, dashes for spaces —
`groceries.md`, `pi-backlog.md`). Same checkbox lines as your todo tool:

```
- [ ] open item
- [~] in progress
- [x] done
```

## Operations

- **Which lists exist?** `list_dir` on `workspace/lists` (missing dir = no lists yet).
- **Read a list:** `fs_read` the file.
- **Create / rewrite:** `write_file` with the full checkbox content.
- **Add / check off / remove one item:** `edit_file` with a uniquely-matching old_string —
  do not rewrite the whole file for a one-line change.
- **Delete a list:** ask the user to delete the file; do not delete it yourself.

These paths are inside your allowed workspace, so reads and edits run without confirmation.

## Rules

- The `todo` tool is NEVER a dumping ground for these lists — it stays your current plan.
  When the user starts WORKING on list items ("let's do the Pi backlog now"), copy the
  relevant few items into the `todo` tool as the plan; check them off in BOTH places as
  they finish.
- When the user says "add X to the <name> list" and no such file exists, create it — do not
  ask which file they meant if only one list name is plausible.
- Keep items one line each. A list is for items, not essays — details the user dictates go
  in the item line; anything longer belongs in a normal note file the item can reference.
- When asked "what's on my lists?", show list names and open-item counts, not every file's
  full content, unless they name one list.
