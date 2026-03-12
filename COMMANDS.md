# esp_storage - Useful commands

Replace `<github-user>` with your GitHub username.

## Add dependency in another project

```bash
idf.py add-dependency "<github-user>/esp_storage>=0.1.0"
```

## Validate component in local project

```bash
cmd /c "call C:\esp\release-v6.0\esp-idf\export.bat && idf.py build"
```

## First push

```bash
git remote add origin https://github.com/<github-user>/esp-storage.git
git pull origin main --allow-unrelated-histories
git checkout --ours LICENSE
git add LICENSE
git commit -m "merge: keep local LICENSE"
git push -u origin main
git push origin v0.1.0
```

## Release update flow

```bash
git add .
git commit -m "feat: update esp_storage"
git tag v0.2.0
git push origin main
git push origin v0.2.0
```

## Use as submodule

```bash
git submodule add https://github.com/<github-user>/esp-storage.git components/esp_storage
git submodule update --init
```

## Flash and monitor when `idf.py` is not in PATH

```bash
cmd /c "call C:\esp\release-v6.0\esp-idf\export.bat && idf.py -p COM6 flash monitor"
```
