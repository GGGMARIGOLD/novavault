# NovaVault

A Steam-style game store written in C++17: accounts, wallet, store, library, game/APK uploads, moderation, admin panel and Discord-style profiles.

## Run it in GitHub Codespaces (easiest)
Click **Code → Codespaces → Create codespace**. It installs everything, builds, starts the server and opens the site in your browser. If the browser doesn't open, use the **Ports** tab and click port 8080.

## Run it on Linux
    sudo apt install g++ make libssl-dev
    make run          # http://localhost:8080   (PORT=3000 ./novavault to change)

Data lives in `data/` (db.json + uploaded files). Back it up.

## Roles and moderation
- The first account on a fresh server is the admin. To make an existing account the admin: `ADMIN_USER=name ./novavault` (the Codespace does this for MARIGOLD).
- **Admin** page: approve or reject submitted games, add credits, make moderators, give badges.
- Games from regular users stay pending until a moderator or admin approves them.

## Profiles
Avatar, banner, status, status message, bio and up to 3 badges. Your profile card sits in the bottom-left corner.

## Notes
- Wallet funds are test money. Run with `NO_DEMO_FUNDS=1` to turn off "Add test funds". Wire a real payment provider into `/api/games/:id/buy` before charging anyone.
- Uploads are held in memory while uploading (2 GB limit). Put it behind HTTPS before going public.
- Sessions are in memory, so everyone logs in again after a restart.
