# Command: review

Run code review using relevant agents on changed files.

## Usage

```
/review
```

## Process

1. Run `git diff --name-only` to find changed files
2. Route files to appropriate agents:

| File Pattern | Agents |
|-------------|--------|
| `controllers/*.ts` | architecture-review + code-review + security-review |
| `services/*.ts` | architecture-review + code-review |
| `content-types/*/schema.json` | architecture-review |
| `routes/*.ts` | security-review |
| `test/*.ts` | testing-review |

3. Generate combined report with findings
