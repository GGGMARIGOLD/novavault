# NovaVault

A Steam-style game store written in C++17: accounts, wallet, store, library, APK/game uploads, and owner-only downloads.

## Build and run (Linux/macOS)
    sudo apt install g++ make libssl-dev curl     # macOS: brew install openssl
    make deps      # downloads cpp-httplib and nlohmann/json
    make run       # http://localhost:8080   (PORT=3000 ./novavault to change)

Data lives in `data/` (db.json + uploaded files). Back it up.

## Notes
- Passwords: PBKDF2-SHA256 with per-user salt. Sessions are in memory (restart = log in again).
- Wallet funds are TEST money ("Add test funds"). Run with `NO_DEMO_FUNDS=1` to disable, and wire a real provider (Stripe) into `/api/games/:id/buy` before charging anyone.
- Uploads are held in memory while uploading (2 GB limit). Put it behind HTTPS (nginx/Caddy) before going public.
- Downloads require login and ownership; developers get paid into their wallet when a game is bought.
