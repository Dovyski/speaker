# Agent skills

`speak/` is the skill that teaches a coding agent to drive `speak.exe`.

Install it by making it visible to the agent — either copy or link the directory
into the skills folder. Linking keeps the two in sync:

```powershell
# Claude Code, user-wide (junction needs no admin rights)
New-Item -ItemType Junction `
  -Path "$env:USERPROFILE\.claude\skills\speak" `
  -Target "C:\Dev\www\claude-speak\skill\speak"
```

Use `.claude/skills/speak` inside a project instead if only that project
should have it. Other agents read skills from their own directories; the file is
plain Markdown with YAML frontmatter and is portable as-is.
