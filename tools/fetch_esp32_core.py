#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_esp32_core.py — 不翻墙安装 arduino-cli 的 esp32 核心

原理:
  1. 读取已下载到本地的 esp32 板包索引 package_esp32_index.json
  2. 解析出 esp32:esp32 最新版本需要的全部工具链压缩包 (xtensa/riscv 编译器、
     esptool、esp32-arduino-libs 等) 和平台包本身的下载地址、SHA256
  3. 把 github.com/espressif/* 地址改写为乐鑫国内镜像
     dl.espressif.com/github_assets/* (直连高速下载)
  4. 下载到 Arduino15/staging/packages/ 并校验 SHA256;
     arduino-cli 之后执行 core install 时会直接使用这些本地文件, 不再联网

用法 (先执行过一次 core update-index --additional-urls ... 生成索引):
    uv run --no-project python tools/fetch_esp32_core.py [--proxy http://127.0.0.1:7890]
    --proxy 仅用于个别非 espressif 域名的兜底下载, 不传则全程直连
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.request

GH_ESPRESSIF = "https://github.com/espressif/"
MIRROR = "https://dl.espressif.com/github_assets/"
GHFAST = "https://ghfast.top/"
# arduino-cli 在 Windows x64 上的工具主机标识, 按优先级
HOSTS_PREFERRED = ["x86_64-mingw32", "i686-mingw32"]


def localappdata():
    return os.environ.get("LOCALAPPDATA", os.path.expanduser("~/AppData/Local"))


def load_index():
    base = os.path.join(localappdata(), "Arduino15")
    p = os.path.join(base, "package_esp32_index.json")
    if not os.path.isfile(p):
        sys.exit(f"索引不存在: {p}\n请先运行: arduino-cli core update-index "
                 f"--additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json")
    with open(p, encoding="utf-8") as f:
        idx = json.load(f)
    # 工具可能定义在 Arduino 默认索引里 (如 dfu-util), 合并进来
    pd = os.path.join(base, "package_index.json")
    if os.path.isfile(pd):
        with open(pd, encoding="utf-8") as f:
            idx2 = json.load(f)
        idx["packages"] = idx.get("packages", []) + idx2.get("packages", [])
    return idx


def version_key(v):
    nums = []
    for seg in v.split("."):
        d = "".join(ch for ch in seg if ch.isdigit())
        nums.append(int(d) if d else 0)
    return nums


def find_latest_esp32_platform(idx):
    for pkg in idx["packages"]:
        if pkg["name"] == "esp32":
            vers = [pf for pf in pkg["platforms"] if "version" in pf]
            if not vers:
                sys.exit("索引中没有 esp32 平台版本")
            return max(vers, key=lambda pf: version_key(pf["version"]))
    sys.exit("索引中没有 esp32 包")


def find_tool(idx, name, version):
    for pkg in idx["packages"]:
        for t in pkg.get("tools", []):
            if t.get("name") == name and t.get("version") == version:
                for host in HOSTS_PREFERRED:
                    for s in t.get("systems", []):
                        if s.get("host") == host:
                            return s
                sys.exit(f"工具 {name}@{version} 没有 Windows 版本")
    sys.exit(f"索引中找不到工具 {name}@{version}")


def rewrite(url):
    """返回按优先级排列的候选下载地址:
    乐鑫国内镜像(仅 espressif 工具链) -> ghfast.top 加速 -> 原始 github 地址"""
    cands = []
    if url.startswith(GH_ESPRESSIF):
        cands.append(MIRROR + url[len(GH_ESPRESSIF):])
    if url.startswith("https://github.com/"):
        cands.append(GHFAST + url)
    cands.append(url)
    return cands


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(urls, dest, retries=4):
    """按候选顺序尝试, 每个地址最多 retries 轮"""
    for rnd in range(1, retries + 1):
        for url in urls:
            try:
                print(f"    尝试: {url}", flush=True)
                with urllib.request.urlopen(url, timeout=90) as r, open(dest + ".part", "wb") as f:
                    shutil.copyfileobj(r, f, 1 << 20)
                os.replace(dest + ".part", dest)
                return True
            except Exception as e:
                print(f"    失败: {e}", flush=True)
                if os.path.exists(dest + ".part"):
                    os.remove(dest + ".part")
        time.sleep(2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy", help="兜底代理, 如 http://127.0.0.1:7890")
    args = ap.parse_args()

    if not args.proxy:
        # 保证纯直连, 不吃环境变量里的代理
        for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
            os.environ.pop(k, None)
    else:
        os.environ["http_proxy"] = args.proxy
        os.environ["https_proxy"] = args.proxy

    staging = os.path.join(localappdata(), "Arduino15", "staging", "packages")
    os.makedirs(staging, exist_ok=True)

    idx = load_index()
    plat = find_latest_esp32_platform(idx)
    print(f"esp32 最新版本: {plat['version']}")

    jobs = []  # (名字, url, 压缩包名, sha256)
    jobs.append(("esp32 平台包", plat["url"], plat["archiveFileName"],
                 plat.get("checksum", "").split(":")[-1]))
    for dep in plat.get("toolsDependencies", []):
        s = find_tool(idx, dep["name"], dep["version"])
        jobs.append((f"{dep['name']}@{dep['version']}", s["url"],
                     s["archiveFileName"], s.get("checksum", "").split(":")[-1]))

    ok, bad = 0, 0
    for name, url, fname, want in jobs:
        dest = os.path.join(staging, fname)
        if os.path.isfile(dest) and (not want or sha256_of(dest) == want):
            print(f"[已有] {name} -> {fname}", flush=True)
            ok += 1
            continue
        print(f"[下载] {name}", flush=True)
        if not download(rewrite(url), dest):
            bad += 1
            print(f"[失败] {name}", flush=True)
            continue
        if want and sha256_of(dest) != want:
            os.remove(dest)
            bad += 1
            print(f"[校验失败, 已删除] {name}", flush=True)
            continue
        print(f"[完成] {name} ({os.path.getsize(dest) // 1048576} MB)", flush=True)
        ok += 1

    print(f"\n结果: 成功/已存在 {ok}, 失败 {bad}")
    if bad:
        print("失败项可用 --proxy http://127.0.0.1:7890 重跑一次兜底")
        sys.exit(1)
    print("全部就绪, 现在可运行 arduino-cli core install esp32:esp32 (不会再联网下载)")


if __name__ == "__main__":
    main()
