# Testing Review Agent - FishPro Backend (Strapi)

## Purpose
Validates test quality and coverage for the Strapi backend.

## When to Run
- After implementing new features
- During code review
- Before merging PRs

---

## MANDATORY Testing Requirement

**After completing ANY implementation task, Claude MUST ask:**

> "Czy chcesz, żebym napisał testy dla tego kodu?"

---

## Test Structure

```
src/test/  (or __tests__/)
├── helpers/
│   └── strapi.ts        # Strapi test instance setup
├── catch.test.ts
├── auth.test.ts
└── ...
```

## Test Pattern for Strapi

```typescript
describe('Catch API', () => {
  let authToken: string;

  beforeAll(async () => {
    // Authenticate test user
    const res = await fetch('http://localhost:1337/api/auth/local', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        identifier: 'test@test.com',
        password: 'testpassword',
      }),
    });
    const data = await res.json();
    authToken = data.jwt;
  });

  describe('GET /api/catches', () => {
    test('Should return 401 without token', async () => {
      const res = await fetch('http://localhost:1337/api/catches');
      expect(res.status).toBe(401);
    });

    test('Should return paginated list', async () => {
      const res = await fetch('http://localhost:1337/api/catches?pagination[page]=1&pagination[pageSize]=10', {
        headers: { Authorization: `Bearer ${authToken}` },
      });
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.data).toBeDefined();
      expect(data.meta.pagination).toBeDefined();
    });
  });

  describe('POST /api/catches', () => {
    test('Should create catch with valid data', async () => {
      const res = await fetch('http://localhost:1337/api/catches', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Authorization: `Bearer ${authToken}`,
        },
        body: JSON.stringify({
          data: {
            latitude: 52.237049,
            longitude: 21.017532,
            privacySettings: 'public',
            date: new Date().toISOString(),
          },
        }),
      });
      expect(res.status).toBe(200);
    });
  });
});
```

---

## What to Test

### High Priority
- [ ] CRUD for each content-type
- [ ] Authentication (401 responses)
- [ ] Authorization (ownership checks)
- [ ] Privacy settings filtering
- [ ] Geo queries (lat/lng/radius)
- [ ] Cache behavior

### Medium Priority
- [ ] Pagination
- [ ] Response mapping
- [ ] Filter combinations
- [ ] File upload/validation

---

## Validation Checklist

- [ ] Tests organized with describe()
- [ ] Descriptive test names ("Should return 401 without token")
- [ ] AAA pattern (Arrange-Act-Assert)
- [ ] Tests are independent
- [ ] Test data cleaned up in afterAll
- [ ] Error cases tested (401, 403, 404, 422)
- [ ] No hardcoded test data (use factories/faker)
