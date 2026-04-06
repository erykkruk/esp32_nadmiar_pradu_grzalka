# Skill: strapi-module

Create a complete Strapi API module with controller, service, routes, and content-type.

## Usage

```
/strapi-module <feature_name>
/strapi-module trophy
/strapi-module fishing-spot
```

## What This Skill Does

Creates a complete Strapi module:
```
src/api/{feature}/
├── content-types/{feature}/
│   └── schema.json
├── controllers/
│   └── {feature}.ts
├── routes/
│   ├── {feature}.ts
│   └── custom-{feature}.ts
├── services/
│   └── {feature}.ts
└── documentation/
    └── 1.0.0/
        └── {feature}.json
```

---

## Step-by-Step Process

### Step 1: Ask for Feature Details

Ask the user:
1. Feature name (singular, kebab-case: e.g., "fishing-spot")
2. Main entity attributes (fields and types)
3. Relations to other content-types
4. Privacy settings needed? (public/friends/private)
5. Geo features needed? (lat/lng)

### Step 2: Create Content-Type Schema

**Location**: `src/api/{feature}/content-types/{feature}/schema.json`

```json
{
  "kind": "collectionType",
  "collectionName": "{features}",
  "info": {
    "singularName": "{feature}",
    "pluralName": "{features}",
    "displayName": "{Feature}",
    "description": ""
  },
  "options": {
    "draftAndPublish": false
  },
  "pluginOptions": {},
  "attributes": {
    "name": {
      "type": "string",
      "required": true
    },
    "user": {
      "type": "relation",
      "relation": "manyToOne",
      "target": "plugin::users-permissions.user"
    }
  }
}
```

**Attribute Types Reference:**
- `string`, `text`, `richtext`
- `integer`, `float`, `decimal`, `biginteger`
- `boolean`
- `datetime`, `date`, `time`
- `enumeration` (with `enum` array)
- `json`
- `media` (with `multiple`, `allowedTypes`)
- `relation` (with `relation`, `target`, `inversedBy`/`mappedBy`)

### Step 3: Create Controller

**Location**: `src/api/{feature}/controllers/{feature}.ts`

```typescript
import { factories } from '@strapi/strapi';

interface {Feature}Response {
  id: number;
  name: string;
  // ... mapped fields
}

const {feature}Mapper = (item: any): {Feature}Response => ({
  id: item.id,
  name: item.name,
  // map relations, images, etc.
});

export default factories.createCoreController('api::{feature}.{feature}', {
  async find(ctx) {
    const { user } = ctx.state;
    if (!user) return ctx.unauthorized('No authorization header was found');

    const { page, pageSize } = ctx.query;

    const { results, pagination } = await strapi.service('api::{feature}.{feature}').find({
      filters: { user: { id: user.id } },
      sort: { createdAt: 'desc' },
      pagination: { page, pageSize },
      populate: ['user'],
    }) as any;

    return {
      data: results.map({feature}Mapper),
      meta: { pagination },
    };
  },

  async findOne(ctx) {
    const { user } = ctx.state;
    if (!user) return ctx.unauthorized('No authorization header was found');

    const { id } = ctx.params;
    const item = await strapi.service('api::{feature}.{feature}').findOne(id, {
      populate: ['user'],
    });

    if (!item) return ctx.notFound('{Feature} not found');

    return {feature}Mapper(item);
  },

  async create(ctx) {
    const { user } = ctx.state;
    if (!user) return ctx.unauthorized('No authorization header was found');

    const { data } = ctx.request.body;
    const response = await strapi.service('api::{feature}.{feature}').create({
      data: { ...data, user: user.id },
      populate: ['user'],
    });

    return {feature}Mapper(response);
  },

  async update(ctx) {
    const { user } = ctx.state;
    if (!user) return ctx.unauthorized('No authorization header was found');

    const { id } = ctx.params;
    const existing = await strapi.service('api::{feature}.{feature}').findOne(id, { populate: ['user'] });
    if (!existing) return ctx.notFound('{Feature} not found');
    if (existing.user?.id !== user.id) return ctx.forbidden('Not allowed');

    return super.update(ctx);
  },

  async delete(ctx) {
    const { user } = ctx.state;
    if (!user) return ctx.unauthorized('No authorization header was found');

    const { id } = ctx.params;
    const existing = await strapi.service('api::{feature}.{feature}').findOne(id, { populate: ['user'] });
    if (!existing) return ctx.notFound('{Feature} not found');
    if (existing.user?.id !== user.id) return ctx.forbidden('Not allowed');

    return super.delete(ctx);
  },
});
```

### Step 4: Create Service

**Location**: `src/api/{feature}/services/{feature}.ts`

```typescript
import { factories } from '@strapi/strapi';

export default factories.createCoreService('api::{feature}.{feature}', {
  // Add custom service methods here
});
```

### Step 5: Create Routes

**Default routes** - `src/api/{feature}/routes/{feature}.ts`:
```typescript
import { factories } from '@strapi/strapi';
export default factories.createCoreRouter('api::{feature}.{feature}');
```

**Custom routes** - `src/api/{feature}/routes/custom-{feature}.ts`:
```typescript
export default {
  routes: [
    // Add custom routes here
  ],
};
```

### Step 6: Permissions (IMPORTANT)

Nowy moduł **nie będzie dostępny** dla użytkowników mobilnych dopóki rola `authenticated` nie dostanie uprawnień `find`/`findOne`.

Uprawnienia są automatycznie dodawane przy starcie Strapi przez `ensureAuthenticatedPermissions()` w `src/index.ts`. Funkcja iteruje po wszystkich `api::` content types i dodaje `find` + `findOne` dla roli `authenticated`.

**Jeśli moduł wymaga dodatkowych akcji** (np. `create`, `update`, `delete` dla authenticated), musisz je dodać ręcznie:
- W panelu admina: **Settings → Users & Permissions → Roles → Authenticated**
- Lub programowo w `src/index.ts` — rozszerz logikę `ensureAuthenticatedPermissions()`

**Po dodaniu nowego modułu wystarczy restart Strapi** — bootstrap automatycznie doda `find` i `findOne`.

### Step 7: Ask About Tests

> "Czy chcesz, żebym napisał testy dla tego modułu?"

---

## Naming Conventions

| Item | Pattern | Example |
|------|---------|---------|
| Feature name | kebab-case (singular) | `fishing-spot` |
| Collection name | kebab-case (plural) | `fishing-spots` |
| Content-type UID | `api::{singular}.{singular}` | `api::fishing-spot.fishing-spot` |
| Controller file | `{singular}.ts` | `fishing-spot.ts` |
| Service file | `{singular}.ts` | `fishing-spot.ts` |

---

## Checklist

- [ ] Content-type schema created with proper attributes
- [ ] Controller with CRUD + auth checks + response mapping
- [ ] Service with factory
- [ ] Default + custom routes
- [ ] User ownership checks on update/delete
- [ ] Explicit populate arrays
- [ ] No `console.log` (use `strapi.log`)
- [ ] Permissions: verify `ensureAuthenticatedPermissions()` in `src/index.ts` covers new module (restart Strapi)
- [ ] Asked user about tests
