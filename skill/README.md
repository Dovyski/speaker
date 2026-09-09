# Agent skills

`speak/` is the skill that teaches a coding agent to drive `speak.exe`: the
calling convention, keeping the daemon warm, captioning an utterance, pointing at
the window it came from, the failure modes, and how to phrase text that is going
to be *heard*.

It is one file — `speak/SKILL.md`, plain Markdown with YAML frontmatter (`name`,
`description`) — so it is portable to any agent that reads skills from a
directory.

## Install it

Make the directory visible to the agent, either by linking it or by copying it.
**Linking keeps the two in sync**, so an update to the repo is an update to the
skill:

```powershell
# Claude Code, user-wide (a junction needs no admin rights)
New-Item -ItemType Junction `
  -Path "$env:USERPROFILE\.claude\skills\speak" `
  -Target "C:\Dev\www\claude-speak\skill\speak"
```

Adjust `-Target` if the repo is cloned elsewhere. Use
`.claude/skills/speak` inside a project instead if only that project should have
it.

Copying works too, and is the right call when the agent runs somewhere the
checkout is not:

```powershell
Copy-Item -Recurse C:\Dev\www\claude-speak\skill\speak `
  "$env:USERPROFILE\.claude\skills\speak"
```

A copy has to be refreshed by hand when the skill changes.

Either way the directory must keep the name `speak` — that is the name the
frontmatter declares and the name the agent invokes.

## Notes

- The skill assumes the checkout is at `C:\Dev\www\claude-speak`; every path in it
  is absolute, so edit them if yours differs.
- It documents `--session` as the way to point at a window, which needs the
  attention panel to have registered that session — see
  [../docs/references/attention-panel.md](../docs/references/attention-panel.md)
  and [../docs/references/claude-code-integration.md](../docs/references/claude-code-integration.md).
- Only the top-level session a user is talking to speaks or points. A subagent
  reports its result as text and lets its orchestrator say it.
