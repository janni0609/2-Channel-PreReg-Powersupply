# Electronic Component Parts Library — Project Plan

Simple personal inventory system for tracking electronic components (article
numbers, descriptions, stock quantities, prices, datasheets), with a web UI
for browsing/searching. Designed for a small collection (up to ~1000 parts),
so it stays deliberately lightweight. Intended to run on an always-on
Raspberry Pi 5.

> Note: This is a separate project from the power supply hardware in this
> repo. It's tracked here as a planning document until it gets its own
> repository (or subfolder).

## Goals

- Track parts with: article number, short description, category, quantity in
  stock, purchase price (if known), datasheet link (if available)
- Web UI to browse, search, filter, and group parts by category
- Inline editing of stock quantity directly in the web viewer
- Add new parts by taking a photo with the Claude app (Pro subscription, no
  extra API costs) and having Claude fill in a standard CSV template, which
  is then imported into the library via the web UI

## Architecture

- **Backend**: FastAPI (Python) + SQLite — simple CRUD API, easy to back up
  (single file database). At ~1000 rows max, SQLite needs no tuning at all.
- **Frontend**: A simple server-rendered or lightweight SPA page, served by
  the backend — table view with search/filter/group and inline quantity edit
- **Hosting**: Runs as a systemd service on the Raspberry Pi 5, reverse-proxied
  via Caddy (gives HTTPS automatically, useful if accessed from a phone)
- **Photo-based import**: No Anthropic API key needed. The user photographs a
  part in the Claude app (Pro subscription) and asks Claude to extract the
  data into a row matching a fixed CSV template (see below). The resulting
  CSV is then uploaded/imported via the web UI.

## Data Model (draft)

| Field | Type | Notes |
|---|---|---|
| id | integer | primary key |
| article_number | text | manufacturer/supplier part number |
| description | text | short description |
| category | text | e.g. "Resistors", "ICs", "Connectors" |
| quantity | integer | editable inline in UI |
| purchase_price | decimal, nullable | optional |
| currency | text, nullable | e.g. EUR |
| datasheet_url | text, nullable | optional link |
| notes | text, nullable | free text |
| created_at / updated_at | timestamp | |

## Phases

### Phase 1 — Backend + Database + CRUD API
- SQLite schema + FastAPI app with endpoints for list/create/update/delete
  parts
- Basic input validation

### Phase 2 — Web UI: browse, search, filter, group
- Table/list view of parts
- Search by article number / description
- Filter by category, in-stock vs. out-of-stock
- Group/collapse by category
- Inline quantity editing (PATCH on change)

### Phase 3 — Manual add/edit
- Form to add/edit a part manually, including price and datasheet link
- Category management (free text or a managed list)

### Phase 4 — Import parts via CSV (extracted using the Claude app)
- Define a fixed CSV template with one column per data field:
  `article_number, description, category, quantity, purchase_price, currency,
  datasheet_url, notes`
- Workflow: take a photo of the part/label/datasheet in the Claude app (Pro
  subscription), ask Claude to fill in a row using this exact template, copy
  the result into a CSV file (one row per part, can batch multiple parts in
  one file)
- Web UI gets an "Import CSV" page:
  - Parses the uploaded file
  - Matches existing parts by `article_number` (updates quantity/fields if
    found, otherwise inserts a new part)
  - Shows a preview/diff before committing
- A blank template file (with header row + one example row) is provided in
  the repo so it's easy to reference/share with Claude when asking for the
  extraction

This avoids any Anthropic API key or per-token billing entirely — all
extraction happens conversationally in the Claude app under the existing Pro
subscription.

### Phase 5 — Raspberry Pi deployment
- systemd service for the FastAPI app
- Caddy reverse proxy for HTTPS (needed for mobile camera access)
- SQLite file backups (e.g. periodic copy to another location)

### Phase 6 — Optional extras
- Tailscale for secure remote access to the inventory from outside the home
  network
- iOS Shortcut to jump straight to the photo-capture page

## Open Questions

- New repo vs. subfolder for this project
- Remote access preference (Tailscale vs. local network only, for now)
- Exact CSV column set/order for the import template — current draft above,
  open to tweaks once Phase 1 schema is in place
