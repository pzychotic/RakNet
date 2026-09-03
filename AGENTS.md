## Agent skills

### Issue tracker

Issues live as markdown files under `.scratch/<feature>/` in this repo. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, used verbatim as label strings. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Coding standards

RakNet uses **no exceptions**: library code in `Source/` reports failure by return value,
and standard-library APIs whose only failure channel is a throw are off-limits there.
Tests are exempt. See `docs/adr/0002-raknet-does-not-use-exceptions.md`.
