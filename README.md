# KV Server (HTTP + LRU Cache + MySQL)

## Build
Requirements: CMake ≥3.18, a C++20 compiler, MySQL/MariaDB client libs (libmysqlclient), and internet (for CMake FetchContent).

### Arch Linux
```bash
sudo pacman -S base-devel cmake gcc mariadb-libs mariadb
# (mariadb provides mysql server if you want local DB)

