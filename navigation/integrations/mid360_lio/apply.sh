#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <mid360-lio-workspace-root>" >&2
  exit 2
fi

target_dir=$(cd "$1" && pwd)
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
patch_file="$script_dir/super_lio_rbt_navigation.patch"
height_patch_file="$script_dir/height-datum-fix.patch"

files=(
  src/super_lio/CMakeLists.txt
  src/super_lio/include/lio/prior_map_localization.h
  src/super_lio/include/rbt_adapter/LIOWrapper.h
  src/super_lio/src/lio/prior_map_localization.cpp
  src/super_lio/src/lio/super_lio.cpp
  src/super_lio/src/rbt_adapter/LIOWrapper.cpp
  src/super_lio/src/rbt_adapter/component.cpp
)

baseline_hashes=(
  f403352bd5013dd1cd4c62c6e4b1ab8be68e923d88156d7030a2021ffdee7067
  771a47bf2f52be3448f4f14e9d2dc35e264d3be344c602c38627ede9d4fd47c8
  0ab5237a4779ddc26fae47dc44e3430539f00d378ef56cdc6af26e8a3ca0bdca
  0d746c9c612b5270fa5a2b5ac62c52401c92f24cdb8b71ac67c0161e6873ce99
  7d77e10f3d85d77bccf352f25b1bc38a4d75f37401994ac78751312fbb81192d
  6e18282ff469fe1be478688124ccebe3ed8c4032aa8695d3fa0b8dbc2309ebaa
  3fcfdde1e9807bc0f90ddcfc42469229e4342203456d506ddf2ad11c2425f712
)

patched_hashes=(
  432725a98b4c941aa525d4bd38927f95a51f9241430df66b4ed3fb6a1721c739
  5fdbb2ccd2cfe5c26bdd907c7cdaa14eb84a78f75484fba05e1d70bb8544786f
  7e3476f1d73f0bb687ce3a28e0bbb4019539fe200e06af67056dca45204fdd6c
  cb88196a042530a19e0b49c75883f4a1595e574f7fdbcaf57d662d6e099c96da
  8f239f527092ef34c95741ea24dfc25a06394db9480a9444815a72fca0e46725
  c659623cbec078655970ff73a37e4130cb28f649859ddd6689a99c75096f1cb9
  410a2f340842cfcc9914083ae81b1d529ecab036e1d9da9f72a72efc8e5a3f87
)

height_fixed_hashes=(
  432725a98b4c941aa525d4bd38927f95a51f9241430df66b4ed3fb6a1721c739
  5fdbb2ccd2cfe5c26bdd907c7cdaa14eb84a78f75484fba05e1d70bb8544786f
  7e3476f1d73f0bb687ce3a28e0bbb4019539fe200e06af67056dca45204fdd6c
  cb88196a042530a19e0b49c75883f4a1595e574f7fdbcaf57d662d6e099c96da
  8f239f527092ef34c95741ea24dfc25a06394db9480a9444815a72fca0e46725
  c659623cbec078655970ff73a37e4130cb28f649859ddd6689a99c75096f1cb9
  07499421eaeafe108090e19a422fe93a27515720be9355b3e9f8eaefa2266b21
)

file_hash() {
  sha256sum "$1" | awk '{print $1}'
}

all_hashes_match() {
  local expected_hashes_name=$1
  local -n expected_hashes="$expected_hashes_name"
  local index
  for index in "${!files[@]}"; do
    [[ -f "$target_dir/${files[$index]}" ]] || return 1
    [[ "$(file_hash "$target_dir/${files[$index]}")" == "${expected_hashes[$index]}" ]] || return 1
  done
}

if all_hashes_match height_fixed_hashes; then
  echo "MID360 Super-LIO navigation patch and base-height fix already applied"
  exit 0
fi

state=""
if all_hashes_match baseline_hashes; then
  state="baseline"
elif all_hashes_match patched_hashes; then
  state="navigation-patched"
else
  echo "source SHA-256 mismatch; refusing to modify $target_dir" >&2
  exit 1
fi

backup_root="$target_dir/.rbt-navigation-backups"
backup_dir="$backup_root/$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -e "$backup_dir" ]]; then
  backup_dir="${backup_dir}-$$"
fi
mkdir -p "$backup_dir"
for file in "${files[@]}"; do
  mkdir -p "$backup_dir/$(dirname "$file")"
  cp -p "$target_dir/$file" "$backup_dir/$file"
done

if [[ "$state" == "baseline" ]]; then
  patch --batch --forward --fuzz=0 -p1 -d "$target_dir" < "$patch_file"
  if ! all_hashes_match patched_hashes; then
    echo "navigation patch result SHA-256 mismatch; original files are backed up in $backup_dir" >&2
    exit 1
  fi
fi

patch --batch --forward --fuzz=0 -p1 -d "$target_dir" < "$height_patch_file"
if ! all_hashes_match height_fixed_hashes; then
  echo "base-height fix result SHA-256 mismatch; source files are backed up in $backup_dir" >&2
  exit 1
fi

echo "MID360 Super-LIO navigation patch and base-height fix applied"
echo "Backup: $backup_dir"
