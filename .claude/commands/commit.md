# Command: commit

Create a well-formatted git commit for FishPro Backend.

## Usage

```
/commit
/commit "Custom commit message"
```

## Commit Message Format

```
<type>(<scope>): <description>
```

### Types

| Type | Description |
|------|-------------|
| `feat` | New feature |
| `fix` | Bug fix |
| `docs` | Documentation only |
| `refactor` | Code change, no feature/fix |
| `perf` | Performance improvement |
| `test` | Adding/updating tests |
| `chore` | Build, config, dependencies |

### Scopes

| Scope | Description |
|-------|-------------|
| `catch` | Catches module |
| `auth` | Authentication |
| `chatbot` | AI chatbot |
| `social` | Friends, invitations, conversations |
| `geo` | Geolocation, maps |
| `cache` | Redis caching |
| `config` | Configuration |
| `db` | Database changes |

### Examples

```
feat(catch): add fishing spot sharing feature
fix(auth): resolve Firebase token validation
perf(cache): increase Redis TTL for public catches
chore(db): add index on catches.date column
```

## IMPORTANT

- **NEVER** add Co-Authored-By in commits
- Run quality checks before committing
- Stage specific files, not `git add .`
- Never commit `.env`, `node_modules/`
