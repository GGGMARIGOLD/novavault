# Vespera

A game store written in C++17: accounts, credits, store, library, game uploads (.exe, .apk, .zip…), reviews, an admin panel, Discord-style profiles and a Profile Shop.

## Run it in GitHub Codespaces (easiest)
Click **Code → Codespaces → Create codespace**. It installs everything, builds, starts the server and opens the site in your browser. If the browser doesn't open, use the **Ports** tab and click port 8080.

## Run it on Linux
    sudo apt install g++ make libssl-dev
    make run          # http://localhost:8080   (PORT=3000 ./vespera to change)

Data lives in `data/` (db.json + uploaded files). Back it up.

## Publishing
Upload .exe, .msi, .apk, .zip and other game files up to 4 GB. Files are sent in 8 MB pieces, so large games upload reliably and a dropped connection resumes instead of starting over.

## Review and the admin panel
- The first account on a fresh server is the admin. To make an existing account the admin: `ADMIN_USER=name ./vespera` (the Codespace does this for MARIGOLD).
- Every game from a non-admin goes to the **review queue** first. Admins and moderators accept it or decline it with a reason; the developer sees the result under Library → My submissions and can resubmit a declined game.
- Admin panel tabs: **Overview** (stats, top games, recent activity), **Review queue**, **Games** (feature, unpublish, change price, delete), **Users** (add or remove credits, badges, moderators, ban and unban), **Activity** (log of every action) and **Settings** (store announcement banner).
- Moderators see only the review queue.

## Profiles
Avatar, banner, status, status message, bio and up to 3 badges. Your profile card sits in the bottom-left corner.

## Updating games
Developers click **Update game** on their game page. Before a game is accepted, edits apply straight away and it goes back to review. After it's accepted, changes (new file, details, cover) become an update that an admin reviews; the current version stays live until the update is accepted. The price of an accepted game is locked: only admins can change it (Admin → Games → Price).

## Credits and the Profile Shop
Games and Profile Shop items are bought with credits. Only admins give out credits (Admin → Users). The Profile Shop sells avatar frames, profile themes, name styles and banner effects; you can try items on before buying.

## Notes
- Credits aren't connected to real money. Wire a payment provider in before selling credits.
- Put it behind HTTPS before going public.
- Sessions are in memory, so everyone logs in again after a restart.
