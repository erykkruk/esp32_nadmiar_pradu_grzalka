# Command: pr

Create a structured pull request for FishPro Backend.

## Usage

```
/pr
```

## Process

1. Run `git log main..HEAD --oneline` to see commits
2. Run `git diff main...HEAD --stat` to see changed files
3. Create PR with template:

```
## Summary
- [1-3 bullet points describing changes]

## Changes
- [List of modified modules/files]

## Testing
- [ ] TypeScript check passes
- [ ] Build passes
- [ ] Manual testing done
- [ ] Tests added/updated
```

## IMPORTANT

- **NEVER** add Co-Authored-By
- Keep PR title under 70 characters
- Use `gh pr create` for creation
