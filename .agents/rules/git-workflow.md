## Git Workflow and Repository Awareness

### Repository Detection Protocol

Use the session's branch, remote, and status checks; inspect their roles rather
than assuming `origin` is a fork or that an `upstream` remote exists.

### Git Operation Rules

- Integration and PR comparisons target `develop`, not `master` or remote HEAD.
- Before a push, recheck remotes and use the destination specified by the
  human. Otherwise push to this fork's `origin`; verify its URL rather than
  assuming a name establishes its role. Ask if the destination is unclear.
- When adapting/superseding another person's PR, verify the original human
  author and credit them with `Co-Authored-By`; skip bot authors.

### Branch Naming Convention

Use `<type>/<short-description>`, where type is `feat`, `fix`, `refactor`,
`docs`, `test`, `chore`, or `perf`, matching PR title prefixes.

### Commit Message Format

Use `<type>: <short summary>` (summary at most 50 characters), with an optional
body containing relevant facts and verification. Types match branch prefixes.
