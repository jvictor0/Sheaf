# Git Workflow

Use this skill when creating branches, rebasing, landing work, or interpreting
the user's Git workflow instructions.

Follow a linear Git doctrine.

`main` stays clean. Do not develop directly on `main` unless explicitly
instructed. Create a worktree for feature work.

Never create merge commits. Never use merge commits as part of normal development or landing.
Always rebase and fast forward.

When merging or rebasing on main, start by pulling the local main worktree and rebase on that.

Definitions:

- `rebase` means rebase the current working branch on `main`.
- `land` means sync+archive with OpenSpec if you're applying an OpenSpec change,
  rebase the working branch on `main`, fast-forward `main` to the working
  branch, identify any services changed by the landed work, delete the working
  branch, delete the worktree, push `main`, then rebuild and redeploy the
  changed services from `main`.

When landing an OpenSpec change, first verify the change is actually ready for
archiving. If artifacts or tasks are incomplete, or the archive workflow would
require confirmation to proceed despite incomplete work, stop and do not
continue with the land until the change is ready to archive.

Before landing, identify the verification required for the affected components
and complete it, including any reviews required by the active workflow. Inspect
the results and tie the evidence to the code being landed. Passing checks for
an earlier version do not establish that later changes work.

After rebasing, inspect the integrated changes and any conflict resolutions.
Rerun checks affected by changed code, dependencies, or integration with main;
reuse earlier evidence only where those inputs are unchanged. If required
verification fails or remains incomplete, stop before fast-forwarding main or
pushing and report the gap.

Landing procedure:

1. Sync and Archive with OpenSpec if you're applying an OpenSpec change.
2. Rebase the working branch on `main`.
3. Verify the rebased result as described above.
4. Fast-forward `main` to the working branch.
5. Identify any services changed by the landed work and note how to rebuild and
   redeploy them.
6. Delete the working branch.
7. Push `main`.
8. For Sheaf, rebuild and redeploy the services changed by the landed work from `main`.
