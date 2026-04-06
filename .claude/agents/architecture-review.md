# Architecture Review Agent - FishPro Backend (Strapi)

## Purpose
Validates Strapi module architecture, layer boundaries, and content-type patterns.

## When to Run
- After creating new API modules
- During code review
- Before major refactoring

---

## Module Structure Pattern

Every Strapi API module MUST follow:

```
src/api/{feature}/
├── content-types/{feature}/
│   └── schema.json            # Content-type definition
├── controllers/
│   └── {feature}.ts           # Controller (route handlers)
├── routes/
│   ├── {feature}.ts           # Default CRUD routes
│   └── custom-{feature}.ts    # Custom routes
├── services/
│   └── {feature}.ts           # Business logic
└── documentation/             # OpenAPI docs (optional)
```

---

## Validation Rules

### 1. Controller Rules

#### Must Extend Core Controller
```typescript
// ❌ VIOLATION - Raw controller without factory
export default {
  async find(ctx) { ... }
}

// ✅ CORRECT - Using Strapi factory
import { factories } from '@strapi/strapi';
export default factories.createCoreController('api::feature.feature', {
  async customAction(ctx) { ... }
});
```

#### Controllers Must Not Contain Heavy Business Logic
```typescript
// ❌ VIOLATION - DB queries in controller
async create(ctx) {
  const items = await strapi.db.query('api::bait.bait').findMany({ ... });
  // 50+ lines of logic...
}

// ✅ CORRECT - Delegate to service
async create(ctx) {
  const { data } = ctx.request.body;
  const { user } = ctx.state;
  return strapi.service('api::feature.feature').createWithRelations(data, user);
}
```

#### Must Validate User State
```typescript
// ❌ VIOLATION - No user check on protected endpoint
async create(ctx) {
  const { data } = ctx.request.body;
  return strapi.service('api::catch.catch').create({ data });
}

// ✅ CORRECT - Check user state
async create(ctx) {
  const { user } = ctx.state;
  if (!user) return ctx.badRequest('No authorization header was found');
  const { data } = ctx.request.body;
  return strapi.service('api::catch.catch').create({ data: { ...data, user: user.id } });
}
```

### 2. Service Rules

#### Must Use Strapi Service Factory
```typescript
// ✅ CORRECT
import { factories } from '@strapi/strapi';
export default factories.createCoreService('api::feature.feature', {
  async customMethod(params) { ... }
});
```

#### Services Must Not Access ctx
```typescript
// ❌ VIOLATION - Service accessing HTTP context
async findFiltered(ctx) {
  const { query } = ctx;
}

// ✅ CORRECT - Receive processed params
async findFiltered(filters, pagination) {
  return strapi.db.query('api::catch.catch').findMany({ where: filters });
}
```

### 3. Route Rules

#### Custom Routes Must Be in Separate File
```typescript
// routes/custom-{feature}.ts
export default {
  routes: [
    {
      method: 'GET',
      path: '/features/custom',
      handler: 'feature.customAction',
      config: {
        policies: [],
        middlewares: [],
      },
    },
  ],
};
```

### 4. Content-Type Rules

#### Must Define All Relations with Proper Types
```json
// ❌ VIOLATION - Missing relation type
"user": {
  "type": "relation",
  "target": "plugin::users-permissions.user"
}

// ✅ CORRECT - Full relation definition
"user": {
  "type": "relation",
  "relation": "oneToOne",
  "target": "plugin::users-permissions.user"
}
```

---

## Validation Checklist

### Controllers
- [ ] Uses `factories.createCoreController()`
- [ ] Validates `ctx.state.user` on protected endpoints
- [ ] Delegates business logic to services
- [ ] Returns mapped/sanitized responses

### Services
- [ ] Uses `factories.createCoreService()`
- [ ] Does not access `ctx` directly
- [ ] Contains reusable business logic

### Routes
- [ ] Default CRUD routes in `{feature}.ts`
- [ ] Custom routes in `custom-{feature}.ts`
- [ ] Proper method, path, handler, config

### Content-Types
- [ ] Valid `schema.json` with all attributes
- [ ] Relations have `relation` type defined
- [ ] Enums have valid `enum` array

---

## Report Format

```
## Architecture Violations Found

### Critical
1. `src/api/catch/controllers/catch.ts:45`
   - Heavy business logic in controller (50+ lines)
   - Fix: Extract to service method

### Warnings
1. `src/api/badge/routes/`
   - Missing custom routes file
   - Fix: Create `custom-badge.ts`

### Passed
- Module structure ✓
- Content-type schemas ✓
```
