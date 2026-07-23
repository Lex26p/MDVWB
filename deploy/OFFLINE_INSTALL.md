# Offline installation on Wiren Board ARM64

The GitHub Actions workflow builds MDVWB inside a Debian 11 ARM64 container. The resulting binary is intended for an ARM64 Wiren Board with glibc 2.31 and the installed `libmosquitto.so.1` runtime library.

## Build the package

1. Commit and push the workflow and installer.
2. Open the repository on GitHub.
3. Open **Actions** → **Build ARM64 Offline Package**.
4. Open the latest successful run.
5. Download the artifact **MDVWB-arm64-offline**.
6. Extract the downloaded ZIP on the Windows computer. It contains `MDVWB-arm64-offline.tar.gz` and its SHA-256 file.

## Transfer with WinSCP

Connect to the controller using SFTP as `root` and upload `MDVWB-arm64-offline.tar.gz` to `/root`.

## Install on the controller

```sh
cd /root
rm -rf MDVWB-arm64
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
./offline-install.sh
```

Edit `/etc/default/mdvwb`, then enable the service:

```sh
systemctl enable --now mdvwb.service
systemctl status mdvwb.service --no-pager
journalctl -u mdvwb.service -n 100 --no-pager
```
