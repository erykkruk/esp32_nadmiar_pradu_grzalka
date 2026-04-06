# Security Review Agent - FishPro Backend (Strapi)

## Purpose
Validates security best practices for authentication, authorization, data validation, and API security in Strapi.

## When to Run
- After implementing authentication features
- When adding new API endpoints
- Before deployment

---

## Validation Rules

### 1. Authentication

#### Protected Routes Must Check ctx.state.user
```typescript
// ❌ VIOLATION - No user check
async create(ctx) {
  const { data } = ctx.request.body;
  return strapi.service('api::catch.catch').create({ data });
}

// ✅ CORRECT
async create(ctx) {
  const { user } = ctx.state;
  if (!user) return ctx.unauthorized('No authorization header was found');
  return strapi.service('api::catch.catch').create({
    data: { ...ctx.request.body.data, user: user.id }
  });
}
```

### 2. Authorization - Resource Ownership

#### Must Verify Resource Ownership Before Mutations
```typescript
// ❌ VIOLATION - Any user can update any catch
async update(ctx) {
  return super.update(ctx);
}

// ✅ CORRECT - Verify ownership
async update(ctx) {
  const { user } = ctx.state;
  const { id } = ctx.params;
  const item = await strapi.service('api::catch.catch').findOne(id, { populate: ['user'] });
  if (!item) return ctx.notFound('Catch not found');
  if (item.user?.id !== user.id) return ctx.forbidden('Not your catch');
  return super.update(ctx);
}
```

### 3. Privacy Settings Enforcement

#### Must Respect privacySettings in Queries
```typescript
// ❌ VIOLATION - Ignoring privacy
const catches = await strapi.service('api::catch.catch').find({ filters: {} });

// ✅ CORRECT - Filter by privacy
const catches = await strapi.service('api::catch.catch').find({
  filters: {
    $or: [
      { privacySettings: 'public' },
      { privacySettings: 'friends', user: { id: { $in: friendsIds } } },
      { privacySettings: 'private', user: { id: user.id } },
    ]
  }
});
```

### 4. Input Validation

#### Never Trust Raw Input
```typescript
// ❌ VIOLATION - Using raw query params in DB queries
const results = await strapi.db.query('api::catch.catch').findMany({
  where: { id: ctx.query.id } // No validation!
});

// ✅ CORRECT - Parse and validate
const id = parseInt(ctx.params.id, 10);
if (isNaN(id)) return ctx.badRequest('Invalid ID');
```

### 5. Sensitive Data

#### Never Log Passwords, Tokens, Full User Objects
```typescript
// ❌ VIOLATION
console.log('user', user); // May include password hash

// ✅ CORRECT
strapi.log.info('User action:', { id: user.id, action: 'create_catch' });
```

#### Never Expose Internal Fields in Responses
```typescript
// ❌ VIOLATION - Returning all user fields
return user;

// ✅ CORRECT - Map to safe fields
return {
  id: user.id,
  nickname: user.nickname,
  rank: user.rank,
  avatar: user.avatar?.url || null,
};
```

### 6. File Upload Security

#### Validate File Types and Size
```typescript
const ALLOWED_TYPES = ['image/jpeg', 'image/png', 'image/webp'];
const MAX_SIZE = 10 * 1024 * 1024; // 10MB

async upload(ctx) {
  const file = ctx.request.files['files.photo'];
  if (!file) return ctx.badRequest('No file present');
  if (!ALLOWED_TYPES.includes(file.type)) return ctx.badRequest('Invalid file type');
  if (file.size > MAX_SIZE) return ctx.badRequest('File too large');
}
```

### 7. Redis Cache

#### Never Cache Sensitive User Data
```typescript
// ❌ VIOLATION - Caching user tokens
await cache.set('user:token:' + userId, token);

// ✅ CORRECT - Cache only public data
await cache.set(hashKey, JSON.stringify({ data: publicCatches, meta: { pagination } }), ttl);
```

### 8. Environment Variables

#### Never Hardcode Secrets
```typescript
// ❌ VIOLATION
const SENDGRID_KEY = 'SG.xxxxx';

// ✅ CORRECT
const SENDGRID_KEY = process.env.SENDGRID_API_KEY;
```

---

## Validation Checklist

- [ ] Protected routes check `ctx.state.user`
- [ ] Resource ownership verified before mutations
- [ ] Privacy settings enforced in list queries
- [ ] Input validated/parsed before use
- [ ] No sensitive data in logs/responses
- [ ] File uploads validate type and size
- [ ] No hardcoded secrets
- [ ] Redis cache keys don't contain sensitive data
- [ ] CORS configured for production
