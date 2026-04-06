# Skill: strapi-patterns

Reference for FishPro Backend patterns, Redis caching, geo queries, and Strapi conventions.

## Strapi Service UIDs

```
api::catch.catch
api::bait.bait
api::lure.lure
api::spiecies.spiecies          # Note: typo in name (legacy)
api::catch-method.catch-method
api::banner.banner
api::chatbot.chatbot
api::configuration.configuration
api::conversation.conversation
api::invitation.invitation
api::usage.usage
plugin::users-permissions.user
```

---

## Response Mapping Pattern

Always map Strapi responses before returning:

```typescript
// User mapper (reusable)
export const userMapper = (user: any) => ({
  id: user.id,
  nickname: user.nickname,
  rank: user.rank,
  avatar: user.avatar?.url || null,
});

// Catch mapper (with relations)
const catchMapper = (item: any) => ({
  ...item,
  image: item.image?.url || null,
  photo: item.image || null,
  bait: item.bait?.name || null,
  lure: item.lure?.name || null,
  user: item.user ? userMapper(item.user) : null,
});

// List mapper
const catchesMapper = (catches: any[]) => catches.map(catchMapper);
```

---

## Pagination Pattern

```typescript
// Query
const { results, pagination } = await strapi.service('api::catch.catch').find({
  filters: { ... },
  sort: { date: 'desc' },
  pagination: { page, pageSize },
  populate: ['bait', 'species', 'lure', 'image'],
}) as any;

// Response format
return {
  data: catchesMapper(results),
  meta: { pagination },
};
```

---

## Redis Cache Pattern

```typescript
// Generate hash key from query object
const hashKey = await strapi.service('api::catch.cache').generateHashKeyFromObject(query);

// Check cache first
const cache = await strapi.service('api::catch.cache').get(hashKey);
if (cache) return JSON.parse(cache);

// Fetch data...
const response = { data, meta: { pagination } };

// Cache with TTL (seconds)
await strapi.service('api::catch.cache').set(hashKey, JSON.stringify(response), 60);

return response;
```

---

## Geo Query Pattern (Redis georedis)

```typescript
// Add location
await strapi.service('api::catch.geo').addLocation({
  longitude: response.longitude,
  latitude: response.latitude,
  member: response.id.toString(),
});

// Query by radius
const locations = await strapi.service('api::catch.geo').getLocations({
  latitude: lat,
  longitude: lng,
  radius: radius,
});

// Use in filter
const geoQuery = locations.length > 0
  ? { id: { $in: locations.map(id => parseInt(id)) } }
  : {};
```

---

## Privacy Settings Pattern

```typescript
const PRIVACY_SETTINGS = ['public', 'friends', 'private'] as const;

// Build privacy-aware query
const buildPrivacyQuery = (userId: number, friendsIds: number[], friendsOnly: boolean) => {
  if (friendsOnly) {
    return {
      $or: [
        { privacySettings: 'friends', user: { id: { $in: friendsIds } } },
        { privacySettings: 'public', user: { id: { $in: friendsIds } } },
      ],
    };
  }

  return {
    $or: [
      { privacySettings: 'public' },
      { privacySettings: 'friends', user: { id: { $in: friendsIds } } },
      { privacySettings: 'private', user: { id: [userId] } },
    ],
  };
};
```

---

## Bait/Lure Auto-Create Pattern

Find or create related entity by name:

```typescript
const findOrCreateRelation = async (
  uid: string,
  name: string
): Promise<number> => {
  let item = await strapi.db.query(uid).findOne({ where: { name } });
  if (!item) {
    item = await strapi.service(uid).create({ data: { name } });
  }
  return item.id;
};

// Usage
if (data.bait) {
  data.bait = await findOrCreateRelation('api::bait.bait', data.bait);
}
```

---

## Filter Query Builder Pattern

```typescript
interface CatchFilters {
  species?: object;
  method?: object;
  bait?: object;
  friends?: object;
  dateRange?: object;
}

const mapArgument = (arg: string): number[] =>
  arg.split(',').map(item => parseInt(item, 10));

const buildFilters = async (query: any, userId?: number): Promise<CatchFilters> => {
  const species = query.species ? { species: { id: mapArgument(query.species) } } : {};
  const method = query.method ? { method: { id: mapArgument(query.method) } } : {};
  const bait = query.bait ? { bait: { id: mapArgument(query.bait) } } : {};

  let dateRange = {};
  if (query.date_from || query.date_to) {
    dateRange = {
      date: {
        ...(query.date_from && { $gte: new Date(query.date_from) }),
        ...(query.date_to && { $lte: new Date(query.date_to) }),
      },
    };
  }

  return { species, method, bait, dateRange };
};
```

---

## File Upload Pattern

```typescript
async upload(ctx) {
  const { user } = ctx.state;
  if (!user) return ctx.unauthorized('No authorization header was found');

  const { id } = ctx.params;
  const photo = ctx.request.files['files.photo'];
  if (!photo) return ctx.badRequest('No files.photo file present');

  const item = await strapi.service('api::catch.catch').findOne(id, { populate: ['image'] });
  if (!item) return ctx.notFound('Catch not found');

  // Upload new file
  const attachments = await strapi.service('plugin::upload.upload').upload({
    data: { refId: id, ref: 'api::catch.catch', field: 'image' },
    files: { path: photo.path, name: photo.name, type: photo.type, size: photo.size },
  });

  // Remove old file if exists
  if (item.image) {
    await strapi.service('plugin::upload.upload').remove(item.image);
  }

  // Update relation
  const [attachment] = attachments;
  return strapi.service('api::catch.catch').update(id, {
    data: { image: attachment.id },
    populate: ['image'],
  });
}
```

---

## Strapi Query Operators

```
$eq     - Equal
$ne     - Not equal
$in     - In array
$notIn  - Not in array
$lt     - Less than
$lte    - Less than or equal
$gt     - Greater than
$gte    - Greater than or equal
$contains    - Contains (case-sensitive)
$containsi   - Contains (case-insensitive)
$or     - Logical OR (array of conditions)
$and    - Logical AND (array of conditions)
```

---

## Available API Scopes

| API | Content-Type | Key Features |
|-----|-------------|-------------|
| catch | Catches (fishing logs) | Geo, privacy, filters, stats, cache |
| bait | Baits | Auto-create by name |
| lure | Lures | Auto-create by name |
| spiecies | Fish species | Reference data |
| catch-method | Fishing methods | Reference data |
| badge-definition | Badge definitions | Gamification (new system) |
| banner | App banners | Promotions |
| chatbot | AI chat (OpenAI) | Conversations |
| configuration | App config | Dynamic settings |
| conversation | Messaging | User-to-user |
| invitation | Friend invites | Social |
| usage | Analytics tracking | Usage data |
| auth | Custom auth | Firebase + Strapi auth |
