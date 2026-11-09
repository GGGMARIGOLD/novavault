# Vespera

A game store written in C++17 (server) and Rust (image tool): accounts, credits, store, library, game uploads (.exe, .apk, .zip…), reviews, an admin panel, Discord-style profiles and a Profile Shop.

## Run it in GitHub Codespaces (easiest)
Click **Code → Codespaces → Create codespace**. It installs everything, builds, starts the server and opens the site in your browser. If the browser doesn't open, use the **Ports** tab and click port 8080.

## Run it on Linux
    sudo apt install g++ make libssl-dev
    make run          # http://localhost:8080   (PORT=3000 ./vespera to change)
    make restart      # stop a Vespera that's already running, rebuild and start again

Data lives in `data/` (db.json + uploaded files). Back it up.

## Rust image tool (img/)
`vespera-img` is a small Rust program the server runs on every uploaded avatar, banner and game cover. It resizes them to the size they're shown at and recompresses them (JPEG, or PNG when the image has transparency), which makes pages much lighter: a 2.6 MB banner becomes about 0.5 MB. It refuses "decompression bomb" images. `make run` and `make restart` build it automatically when Rust is installed; without Rust, Vespera still works and keeps uploads as they are.

## Speed
Pages and data are gzip-compressed, the page is kept in memory and revalidated with an ETag, and images carry a version in their URL so browsers cache them until they change.

## Publishing
Upload .exe, .msi, .apk, .zip and other game files up to 4 GB. Files are sent in 8 MB pieces, so large games upload reliably and a dropped connection resumes instead of starting over.

## Review and the admin panel
- The first account on a fresh server is the admin. To make an existing account the admin: `ADMIN_USER=name ./vespera` (the Codespace does this for MARIGOLD).
- Every game from a non-admin goes to the **review queue** first. Admins and moderators accept it or decline it with a reason; the developer sees the result under Library → My submissions and can resubmit a declined game.
- Admin panel tabs: **Overview** (stats, top games, recent activity), **Review queue**, **Games** (feature, unpublish, change price, delete), **Users** (add or remove credits, badges, moderators, timed or permanent bans with a reason, unban), **Activity** (log of every action) and **Settings** (store announcement banner, support email shown on the ban screen).
- Moderators see only the review queue.

## Profiles
Display name, @username, avatar, banner, status, status message, bio and badge icons (hover a badge to see what it means). Anyone can change their display name in profile settings; only admins can change usernames (Admin → Users → Change username). Your profile card sits in the bottom-left corner.

## Updating games
Developers click **Update game** on their game page. Before a game is accepted, edits apply straight away and it goes back to review. After it's accepted, changes (new file, details, cover) become an update that an admin reviews; the current version stays live until the update is accepted. The price of an accepted game is locked: only admins can change it (Admin → Games → Price).

## Credits and the Profile Shop
Games and Profile Shop items are bought with credits. Only admins give out credits (Admin → Users). The Profile Shop sells 42 items: avatar frames, profile themes, name styles and banner effects. You can try items on before buying.

## Owner Vault
Admin → 👑 Owner Vault holds 12 exclusive animated cosmetics (Sovereign Halo, Event Horizon, Hellfire, Throne Room, Galaxy Core, The Abyss, Liquid Gold, Overlord, Starlight, Meteor Storm, Gold Rain, Warp Speed). Only admins can wear them; they can't be bought. There is also an Owner badge admins can give out.

## Playful buttons
Dangerous actions use hold-to-confirm (delete, ban, decline) or slide-to-confirm (deleting from a game page). Purchases and approvals throw confetti. Inspired by [Evil Buttons](https://github.com/radiumcoders/evil-buttons) (Apache-2.0); see NOTICE.

## Switch accounts
Click ⇅ on your profile card (bottom left) to add up to 3 accounts and switch between them with a click. You sign in to each account once; the browser keeps only secure login tokens, never passwords. Logging out of one account moves you to the next saved one. Banned accounts can switch away from their ban screen.

## Bans
Admins ban from Admin → Users → Ban…: pick a reason (or write one), add an optional message, choose 1 hour, 1 day, 3 days, 7 days, 30 days or permanent, then hold to confirm. The banned person sees a suspension screen with the reason, the exact end time, a live countdown, and a case ID. Timed bans end by themselves and their games return to the store.

## Social
- **Comments** on games in the store. Authors and admins/moderators can delete comments; the developer is tagged on their own comments.
- **Follow** anyone (one-way). Profiles show followers, following and friends.
- **Friends** are two-way: send a request, the other person accepts. The Friends page lists requests, friends (online first) and sent requests, and lets you add someone by username.
- **Partner badge**: a golden verified seal that only admins can give (Admin → Users → Badges).

## Notes
- Credits aren't connected to real money. Wire a payment provider in before selling credits.
- Put it behind HTTPS before going public.
- Sessions are in memory, so everyone logs in again after a restart.
