# Electronic Component Parts Library — Project Plan

Personal inventory system for tracking electronic components (article numbers,
descriptions, stock quantities, prices, datasheets), with a web UI for
browsing/searching and a photo-based "add part" workflow powered by the Claude
API. Intended to run on an always-on Raspberry Pi 5.

> Note: This is a separate project from the power supply hardware in this
> repo. It's tracked here as a planning document until it gets its own
> repository (or subfolder).

## Goals

- Track parts with: article number, short description, category, quantity in
  stock, purchase price (if known), datasheet link (if available)
- Web UI to browse, search, filter, and group parts by category
- Inline editing of stock quantity directly in the web viewer
- Add new parts by taking a photo (e.g. of a component, label, or datasheet)
  and have structured data extracted automatically via the Claude API

## Architecture

- **Backend**: FastAPI (Python) + SQLite — simple CRUD API, easy to back up
  (single file database)
- **Frontend**: React + Vite SPA served by the backend, or as a simple PWA so
  it can be added to a phone home screen for quick photo capture
- **Hosting**: Runs as a systemd service on the Raspberry Pi 5, reverse-proxied
  via Caddy (gives HTTPS automatically, useful for camera access on mobile
  browsers which require a secure context)
- **Photo extraction**: Backend endpoint that sends the uploaded image to the
  Anthropic API (vision input) with a prompt asking for structured JSON
  (article number, description, category guess, value/specs, datasheet URL if
  visible), then pre-fills an "add part" form for the user to review/confirm

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

### Phase 4 — Photo-based part entry (Claude API)
- Mobile-friendly capture page (PWA) that uploads a photo
- Backend sends image to Claude API (vision) with an extraction prompt,
  returns structured JSON (article number, description, value/specs, possible
  category, datasheet link if visible on packaging)
- Pre-filled "review & confirm" form before saving to the database

**Anthropic API setup & cost notes:**
- Requires a separate Anthropic **Developer Console** account
  (console.anthropic.com) and its own API key — this is billed separately
  (pay-as-you-go per token) and is **not** included in a Claude.ai Pro/Max
  subscription
- Recommended model: **Claude Haiku 4.5** (`claude-haiku-4-5`) — cheap and
  sufficient for reading part markings/labels
- A photo (~1,200–1,600 tokens) + short prompt + small JSON response works out
  to roughly **$0.002 per image** on Haiku 4.5 (~$2 for 1,000 parts added by
  photo) — negligible for this use case

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
- Rough scale of inventory (tens vs. hundreds vs. thousands of parts) — mainly
  affects how much UI polish for search/filter is worth building up front
- Whether to set up the Anthropic API key now (for Phase 4) or defer until
  Phases 1–3 are working
