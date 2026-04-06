# Code Review Agent - FishPro Backend (Strapi)

## Purpose
Validates code quality, error handling, typing, and conventions for the Strapi backend.

## When to Run
- Before committing code
- During code review
- After implementing new features

---

## Validation Rules

### 1. Error Handling

#### Must Use ctx Error Methods
```typescript
// ❌ VIOLATION - Throwing raw errors
throw new Error("User not found");

// ✅ CORRECT - Using Strapi ctx methods
return ctx.badRequest('Invalid input');
return ctx.notFound('Resource not found');
return ctx.forbidden('Not allowed');
return ctx.unauthorized('No authorization header was found');
```

### 2. Type Safety

#### Avoid @ts-ignore
```typescript
// ❌ VIOLATION - Suppressing type errors
// @ts-ignore
const { results, pagination } = await strapi.service('api::catch.catch').find({ ... });

// ✅ CORRECT - Proper typing
const response = await strapi.service('api::catch.catch').find({ ... }) as {
  results: any[];
  pagination: { page: number; pageSize: number; total: number; pageCount: number };
};
```

#### Must Type Function Parameters
```typescript
// ❌ VIOLATION - Untyped params
const catchMapper = (item) => { ... }

// ✅ CORRECT - Typed params
interface CatchItem {
  id: number;
  image?: { url: string } | null;
  bait?: { name: string } | null;
  // ...
}
const catchMapper = (item: CatchItem) => { ... }
```

### 3. No console.log in Production
```typescript
// ❌ VIOLATION
console.log('fishCatch', fishCatch);

// ✅ CORRECT - Use strapi.log
strapi.log.info('Processing catch:', { id: fishCatch.id });
strapi.log.error('Failed to process:', error);
strapi.log.debug('Query params:', params);
```

### 4. No Hardcoded Values
```typescript
// ❌ VIOLATION
const maxPageSize = 100;
if (status === 'public') { ... }

// ✅ CORRECT - Use constants
const PRIVACY_SETTINGS = ['public', 'friends', 'private'] as const;
const MAX_PAGE_SIZE = 100;
const CACHE_TTL = 60; // seconds
```

### 5. Naming Conventions

| Type | Convention | Example |
|------|------------|---------|
| Files | kebab-case | `custom-catch.ts` |
| Variables | camelCase | `geoQuery` |
| Functions | camelCase | `getCatches()` |
| Constants | SCREAMING_SNAKE | `MAX_PAGE_SIZE` |
| Types/Interfaces | PascalCase | `CatchFilters` |
| Content-types | kebab-case | `catch-method` |
| API uid | `api::{singular}.{singular}` | `api::catch.catch` |

### 6. Service References

#### Must Use Proper Strapi UID Format
```typescript
// ❌ VIOLATION - Wrong UID
strapi.service('catch').find();

// ✅ CORRECT - Full UID
strapi.service('api::catch.catch').find();
strapi.db.query('plugin::users-permissions.user').findOne({ ... });
```

### 7. Populate Pattern

#### Must Specify Populate Explicitly
```typescript
// ❌ VIOLATION - Implicit populate (loads everything)
const item = await strapi.service('api::catch.catch').findOne(id);

// ✅ CORRECT - Explicit populate
const item = await strapi.service('api::catch.catch').findOne(id, {
  populate: ['bait', 'species', 'lure', 'image', 'user', 'user.avatar']
});
```

### 8. Response Mapping

#### Must Map Responses (no raw Strapi objects)
```typescript
// ❌ VIOLATION - Returning raw Strapi response
return results;

// ✅ CORRECT - Mapped response
return {
  data: catches.map(catchMapper),
  meta: { pagination }
};
```

---

## Validation Checklist

- [ ] No `console.log` statements
- [ ] No `@ts-ignore` without justification
- [ ] All function parameters typed
- [ ] Uses `ctx.badRequest/notFound/forbidden` for errors
- [ ] Uses `strapi.log` for logging
- [ ] No hardcoded strings/numbers
- [ ] Proper Strapi UID format (`api::feature.feature`)
- [ ] Explicit populate arrays
- [ ] Response mapping on all endpoints
- [ ] camelCase variables, PascalCase types
