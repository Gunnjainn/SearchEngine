# Contributing

## Branching Strategy — Trunk-Based Development

We follow **trunk-based development** with short-lived feature branches.

### Rules

1. **`main` is the trunk.** It must always be in a buildable, testable state.
2. **Short feature branches only.** Branch off `main`, keep the branch alive for
   **at most 1–2 days**, then merge back.
3. **Branch naming:** `<initials>/<short-description>` (e.g. `jy/bm25-scoring`).
4. **Every merge goes through a PR.** The other partner must review and approve
   before merging.
5. **Rebase on `main`** before opening a PR to keep history linear.
6. **Squash-merge** to keep the commit log clean on `main`.

### Workflow

```
git checkout main
git pull origin main
git checkout -b jy/my-feature

# ... do work, commit small and focused ...

git rebase origin/main
git push -u origin jy/my-feature
# Open PR → partner reviews → squash-merge → delete branch
```

### Commit Messages

- Use imperative mood: `Add BM25 scoring`, not `Added BM25 scoring`.
- Keep the subject line under 72 characters.
- Reference issue numbers where applicable.

### Code Review Checklist

- [ ] Does it compile / pass linting?
- [ ] Are there unit tests for new logic?
- [ ] Are the two contracts (Document JSONL, Engine Query API) preserved?
- [ ] Is the commit small and focused?
