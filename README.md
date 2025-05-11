# nntm - No Nonsense Todo Manager

Interactive terminal todo manager.

Supports:

- context (grouping by `@type` and viewing them separately),
- priority (A-Z),
- dates (addition and completion),
- toggling between un/completed,
- and sorting (priority, date),
- grouping by un/completed,

## Motivation

I use the Android app _Markor_ for my todos on my phone, and it uses a very simple and straight forward format. As my phone is synced with my computer via _Syncthing-fork_, I thought of writing this software to be able to handle the same todo file also on the computer. I wanted a simple way to manage my todos in the terminal, and this is what I came up with.

This todo manager being compatible with _Markor_, does not prevent you from using it without _Markor_. All data is stored in a very simple text file.

## Usage

```bash
nntm <todo-file> [--exec /path/to/script.sh]
```

- `todo-file`: Path to your plain text todo list.
- `--exec`: _(optional)_ Script to run when adding or completing todos.

Use keyboard shortcuts to navigate, add, complete, sort, or archive tasks. Press `?` inside the viewer to see all available keys (see section _Interface_ below).

Example:

```bash
nntm ~/tasks/todo.txt --exec ~/hooks/notify.sh
```

## Interface

Here’s your key table split into categories for clarity, with appropriate headings:

### 🔍 Navigation

| Key | Action                               | Notes                                 |
| --- | ------------------------------------ | ------------------------------------- |
| `j` | Move down                            | Navigate to next visible item         |
| `k` | Move up                              | Navigate to previous visible item     |
| `h` | Switch to previous context (`@type`) | Cycles backward through types         |
| `l` | Switch to next context (`@type`)     | Cycles forward through types          |
| `@` | Jump to context                      | Prompts for `@type` name to switch to |

When switching to context using `@`, if no todos exist in that context, the list will be empty. You can add a new todo using `n` to create a new todo in that context.

### ✅ Task Management

| Key     | Action                                   | Notes                              |
| ------- | ---------------------------------------- | ---------------------------------- |
| `SPACE` | Toggle completed state                   | Marks/unmarks task as complete     |
| `s`     | Set or clear priority (on selected item) | Skips completed items              |
| `t`     | Change type (context) of selected item   | Prompts for new `@type` name       |
| `n`     | Add new todo                             | Adds item to current group/context |
| `A`     | Archive completed todos                  | Appends to `todo.archive.txt`      |

`A` clears the todo file of completed todos and appends them in a file called `todo.archive.txt` in the same directory as the original file. This file is created if it does not exist. (This follows the way _Markor_ does it.)

### 🔃 Sorting & Grouping

| Key | Action                         | Notes                                  |
| --- | ------------------------------ | -------------------------------------- |
| `p` | Sort by priority (A → Z)       | Empty priority appears last            |
| `P` | Sort by priority (Z → A)       |                                        |
| `d` | Sort by date (oldest first)    | Uses `YYYY-MM-DD` format               |
| `D` | Sort by date (newest first)    |                                        |
| `g` | Group by uncompleted/completed | Keeps current sort order within groups |
| `G` | Restore original file order    | Discards sort/grouping changes         |

### 🛠 Miscellaneous

| Key | Action            | Notes                     |
| --- | ----------------- | ------------------------- |
| `?` | Show help overlay | Press any key to close it |
| `q` | Quit              | Exits the viewer          |

## The virtual category `@all`

The all category is a virtual context that displays all todos across all @types. It supports full operations, including navigation, sorting, grouping, adding new items, toggling completion, and setting priorities, just like real categories.

Adding a new todo while in the `@all` context will mark it with the actual type `@all`. A todo entry without any `@type` set will be treated as `@all` by default.

## `--exec` Hook

You can optionally pass a script to be executed when todos are **added** or **toggled un/completed**. This is done using the `--exec` command-line argument:

```
nntm todo.txt --exec /path/to/script.sh
```

### Behavior:

- When a todo is **added**, the script is invoked with the following as argument:

  ```
  Added: <text>
  ```

- When a todo is **marked as completed**, the script is invoked the following as argument with:

  ```
  Completed: <text>
  ```

### Requirements:

- The script must be executable.
- It must accept a **single argument** (the prefixed todo text).
- The viewer does **not wait** for the script to finish (non-blocking, runs in a forked child).

### Example:

```bash
#!/bin/bash
# /home/f/hooks/notify.sh

echo "Triggered: $1" >> ~/.nntm_log
```

Then run:

```bash
nntm todo.txt --exec /home/f/hooks/notify.sh
```

This enables integration with external tools like notifications, logging, syncing, or webhooks.

## Limitations

- _Markor_ todo files have context (`@`) and project (`+`). The latter is not implemented here.

## Todo

- Cmd arg for specifying archive file.
- Ability to edit a todo (with prompt and with custom editor).

## Trivia

Mostly generated by AI with a lot of tweaking. It was generated step by step, adding one feature after another in isolation. I sort of know my way around C and this gave me some memories, but I just do not remember the syntax and many of the details. This took me half a day, and I expanded with more features than I initially intended.
