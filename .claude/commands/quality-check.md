# Command: quality-check

Run comprehensive code quality checks for FishPro Backend.

## Usage

```
/quality-check
```

## Steps

### Step 1: TypeScript Check

```bash
cd fishPro-backend/app && npx tsc --noEmit
```

### Step 2: Build

```bash
cd fishPro-backend/app && npm run build
```

### Step 3: Verify No console.log

Search for `console.log` in `src/` and flag any found.

## Quick Command

```bash
cd fishPro-backend/app && npx tsc --noEmit && npm run build
```
