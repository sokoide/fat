# FAT実装 コードレビュー報告と改善計画

日付: 2026-10-05
方法: 4エージェント並列レビュー（仕様準拠 / メモリ安全性 / 設計 / ビルド・テスト）+ clang 21によるASan+UBSan実証。破損イメージ7種とミニハーネスで再現確認（成果物: `/tmp/fatreview/`）。

---

## 総評

FAT12のコアデコード（`fat_get_fat`の12bit展開・`DirectoryEntry`構造体レイアウト・領域導出）は仕様どおりで正しい。一方で (1) 削除エントリとチェーン終端の走査規則が仕様違反、 破損イメージに対する防御がゼロで大半が即SEGV、 グローバル状態＋モデル/ビュー同居という構造的負債が高度化をブロック、の3層の問題がある。更にレビュー時点で`make test`は失敗状態だった。

## 0. 当時進行中の障害

| 状況 | 根源 |
|---|---|
| `make test`がexit 134で失敗 | `demof12.fat`がmtools 4.0.48再生成でOEM=`MTOO4048`。`testmain.c:13`は`MTOO4032`を期待 → OEMアサートは削除する（ジオメトリassert群が実質をカバー） |
| `.gitignore`が機能せず`tags`がgit管理下 | `tag`は`tags`のtypo |

## 1. 仕様違反（正確性）

- **H1. 削除エントリで走査打ち切り** — `fat.c:340` `fat.c:366` `fat.c:308` `testmain.c:74` `testmain.c:107`: `name[0]==0xE5`は`continue`が正しい（0x00のみ走査終了）。削除履歴のあるイメージでls結果が欠ける。
- **H2. EOC判定のハードコード `cluster < 0xF00`** — `fat.c:298` `fat.c:358` `testmain.c:99`: 正しい`fat_is_end_of_cluster`（`fat.c:474`）が未使用・`fat.h`に宣言なし。0xF00–0xFF7（予約/bad）を正常終端扱い、の大容量FAT12でチェーン途中打ち切り、FAT16/32では全く誤り。
- **M5. `main.c:135-137` 行順序バグ** — `ls /dir2/subbdir1`（typoも）がdir2のリストを表示。137行目はdead store。
- **M2. `fat_print_fat12`ループ上限`fat.c:192`** — `tableCount`（FAT個数）を`tableSize16`（FATセクタ数）と取り違え、FAT全1024エントリ中341件のみ表示。
- **M1. FATタイプ判定 `fat.c:60-67`** — `totalSectors32`未参照、閾値は`< 4085`が仕様（`<=`は不一致）、FAT16/32と判定されても`fat_get_fat`は常時12bitデコード。未対応なら明示エラーに。
- **M3. LFN未処理** — 属性0x0Fエントリが`callback_ls`（`main.c:31`）で生バイトを「V」として表示。両イテレータでスキップが必要。
- **M4. パス解決 `main.c:50-62`** — 中間コンポーネント不在時、戻り値0がルートと衝突し以降トークンをルートから再探索。空ファイル（正当にcluster 0）が"path not found"。センチネル0の二義性が根源。
- **低** — 拡張子末尾スペース未除去（`FOO.A  `）、0x05頭バイト（漢字）未対応、`'.'`/`'..'`解決不能、`fat_print_info`の切り捨て除算。

**検証済み正常**: `fat_get_fat`偶数/奇数の12bit復号、`DirectoryEntry`オフセット（sizeof=32）、`FatBS`先頭36バイト、ルートdirセクタ切上げ（`fat.c:56-58`）、`(attr & 0x18) == 0`の拡張子判定。

## 2. メモリ安全性（ASan/UBSan実証済み。正常イメージでは無検出）

| # | 箇所 | シナリオ |
|---|---|---|
| 1 | `fat.c:37,44` | `totalSectors16*bytesPerSector`のintオーバーフロー（UBSan実証）→size_tラップ→巨大malloc（ASan実証） |
| 2 | `fat.c:53-61` | 領域計算無検証。`reservedSectorCount=0xFFFF`のみでinit成功→SEGV |
| 3 | `fat.c:401-418` | `fat_set_entry_name`が境界なし書き込み。ベース>8/拡張>3で`name[11]`超過（スタック破壊実証） |
| 4 | `fat.c:524` | 非NUL終端`name[11]`への`strlen`（heap外読取33B実証） |
| 5 | `main.c:69-85` | `cat_file_for_cluster`にEOC検査なし→fileSize水増しのみでバッファ外memcpy SEGV（実証）。1024固定もクラスタサイズ≠1024で破綻 |
| 6 | `fat.c:358`等 | サイクル検出なし。FAT[8]=5,[5]=6,[6]=5で無限ループ（実証） |
| 7 | `fat.c:26-27,50` | init失敗時`_fat_bs`が死亡スタックフレームを指す→SEGV（実証）。`fat_unint`は`_fat_bs`をNULL化せず |

**中:** 再init時リーク（`main.c`は`fat_unint`未呼び出し）、`strcpy(tmp_path[64])`（`main.c:44`）、`fat_get_entry_name`の`len<12`は1バイト足りない（8.3名は13バイト必要→`len<13`）、0x55AA未検証（ゼロ埋めでもinit成功・実証）、クラスタ0/1/freeで`(cluster-2)`がuint32ラップ→FAT領域を誤解釈（実証）。

**低:** `_fat_buffer`/`_fat_bs`が非staticで外部リンケージ（duplicate symbol実証）、`fat.h`に`fat_is_*`等のプロトタイプ欠落、`void*`算術はGNU拡張。

**安全確認済み:** `bytesPerSector=0`は除算到達前に`fread`失敗で拒否（エラーメッセージは誤解招く）、二重`fat_unint`は安全、`fread(512,1)`の部分読みなし。

## 3. 設計 — 高度化の阻害要因

1. **グローバル可変状態**（`fat.c:14-22`）→ `fat_ctx_t *ctx`を全関数第一引数に。複数イメージ・テスト独立性・解放順序問題が解消。
2. **モデル/ビュー同居** — `fat.c`の47%が`fat_print_*`族、コアがcolor.hに依存する逆流 → `fat_core.c`（stdio/color依存ゼロ）/`fat_dump.c`に分割。
3. **エラー処理** — ライブラリ内`fprintf(stderr)`+bool → `fat_result_t` enum + `fat_strerror()`。M4のセンチネル二義性の根本解: `fat_result_t fat_lookup(ctx, dir, path, fat_dirent_t *out)`。
4. **隠れた副作用** — `fat_get_cluster_for_entry`が検索なのに呼び出し元entryを上書き。
5. **命名** — `fat_unint`→`fat_uninit`、`ignoreInFAT12`→`firstClusterHigh`、`startingClusterNumber`→`firstClusterLow`、`uintN_t`統一。

**デッドコード:** `fat.c:280-292`の到達不能ルート分岐、未使用の`check_null`/`clbg`/`cl_test`/`spike_strtok`、コメント塊。`FatExtBS32`のコメント块のみFAT32対応のため温存。

## 4. パフォーマンス

- 丸ごとRAM載せは720KBでは可、FAT32数GBで破綻 → フェーズ5で解決。
- `_callback_find_entry`は一致後も走査が止まらない → 停止契約へ。
- ダンプ系の1バイト毎`printf`。`cat_file_for_cluster`の`%s`はNUL打ち切り → バイナリ安全な読み出しAPIの欠如。

## 5. ビルド・テスト

**Makefile（実証済み）:** ヘッダ依存追跡なし（`touch fat.h`で再コンパイルゼロ）→ `-MMD -MP`+`-include`。`$(TARGET): build`パターンで毎回再リンク。`OUTDIR = ~/tmp/hoge`はリポジトリ外共有ディレクトリで`clean`が他の作業を消す → `OUTDIR ?= build`。`rm $(IMG)`に`-f`なし。`CXX`→`CC`。`-Wextra -Wshadow`は警告12件のみで導入容易。

**テスト:** `cat_file`のマルチクラスタ読み（`test_5kb.txt`=4962B=5クラスタのfixtureあり）と`cluster_for_path`がゼロテスト。assertのみで進行表示なし → `RUN()`マクロ。fixtureは「コミット済みかつ再生成可能だが非再現」の矛盾 → 凍結+CI diff か untrack+生成依存 のどちらかへ。クラスタ8/11等のマジックナンバーは1テストに集約。

**ツーリング:** ASan/UBSanテストターゲット（正常fixtureでgreen実証済み）、`make check`、`scan-build`、mtoolsをオラクルに`mdir -b`/`mtype`との差分assert、ブートセクタbit反転マイクロファズ。

## 6. 高度化ロードマップ

- **フェーズ0（全前提）:** コンテキスト化、`fat_core`/`fat_dump`分割、エラーenum、`fat_dirent_t`（`name[256]`でLFN前提）、センチネル整理（`FAT_CLUSTER_ROOT`明示的命名）。
- **フェーズ1 堅牢性:** BPB検証（`bytesPerSector∈{512,1024,2048,4096}`、`sectorsPerCluster`2の冪、`totalSectors16==0`なら`totalSectors32`、積≤実ファイルサイズ、0x55AA）、範囲検査、`fat_is_end_of_cluster`統一+反復上限=クラスタ数。
- **フェーズ2 FAT16/32:** `fat_get_fat`タイプ別ディスパッチ（FAT32は`& 0x0FFFFFFF`マスク必須）、FAT32ルートはBPB[44]`rootCluster`のチェーン走査、`FatExtBS32`実体化、FSInfo読み取り、アクティブFAT選択。
- **フェーズ3 LFN:** 0x0F連続エントリ結合、UTF-16→UTF-8、短名チェックサム検証、LFN優先・8.3フォールバック。
- **フェーズ4 書き込み:** FAT書き込み（12bitはread-modify-write、両FATコピー）、クラスタ確保/解放、ディレクトリスロット探索、FSInfo更新、DOSタイムスタンプ。詳細設計は§12。
- **フェーズ5 I/O抽象化:** `fat_io_t{read/write/size}`差し替え+セクタキャッシュ。丸ごとRAMは`fat_io_mem`実装の一つに。FILE*・ブロックデバイス・FUSEバックエンドが並列実装可能。

## 7. 修正優先順位

1. **即時:** OEMアサート削除（`make test`復活）、`.gitignore`の`tags`修正
2. **小差分の実害解消:** 0xE5 continue → `fat_is_end_of_cluster`使用 → `main.c`行順序 → `len<13` → LFNスキップ
3. **防御:** BPB検証・範囲検査・`fat_set_entry_name`境界・`strlen`→バイト比較・サイクル上限、`-fsanitize`ターゲットと`-MMD`
4. **フェーズ0リファクタ** → 以降フェーズ順（別途承認）

## 8. 修正ウェーブ1の分担とAPI契約（本日実施）

ファイル所有権で衝突を回避（`fat.c`/`fat.h`、`main.c`、`Makefile`/`testmain.c`/`color.*`/`.gitignore`）。

**`fat.h` の新規公開契約（全員がこの署名に合わせる）:**

```c
#define FAT_CLUSTER_ROOT 0u
#define FAT_CLUSTER_NOT_FOUND 0xFFFFFFFFu

void  fat_uninit(void);                      /* fat_unintから改名。_fat_bs等もNULL化 */
bool  fat_is_broken(uint32_t cluster);
bool  fat_is_end_of_cluster(uint32_t cluster);
uint32_t fat_get_cluster_size(void);         /* bytesPerSector * sectorsPerCluster */
bool  fat_set_entry_name(DirectoryEntry* entry, const char* name); /* 8.3非表現はfalse */
uint32_t fat_get_cluster_for_entry(uint32_t parent_cluster, DirectoryEntry* entry);
                                            /* 不一致時 FAT_CLUSTER_NOT_FOUND */
void* fat_get_cluster_ptr(uint32_t cluster); /* 範囲外は NULL */
```

- fix-core: `fat.c`/`fat.h` — H1/H2/M2/M3、安全性防御（BPB検証・範囲検査・サイクル上限・`len<13`・`strlen`排除・`fat_set_entry_name`境界・static化・`fat_unint`改名・デッドコード削除）、上記API契約の実装、`-Wextra`警告の解消
- fix-app: `main.c` — M5行順序、`tmp_path`境界、`cat_file_for_cluster`のEOC/クラスタサイズ/`fwrite`化、`FAT_CLUSTER_NOT_FOUND`扱い、中間コンポーネントのATTR_DIRECTORY検証、`fat_uninit`呼び出し
- fix-build: `Makefile`/`testmain.c`/`color.c|h`/`.gitignore` — 依存追跡・OUTDIR→`build`・再リンク解消・`rm -f`・`CC/CFLAGS`・`test-san`(ASan/UBSan)・`check`ターゲット、OEMアサート削除・`RUN()`マクロ・`fat_is_end_of_cluster`への置換・多クラスタ読みテスト追加、`cl_test`/`clbg`/`spike_strtok`削除
- 統合検証（fix-complete）: 全ファイル揃った後に`make check`・ASan・破損イメージ再実験（`/tmp/fatreview`）

コミットはユーザー指示まで行わない。

## 9. ウェーブ1実施結果（2026-10-05 完了）

全チェック合格。最終状態:

- `make check`: クリーンビルド警告ゼロ（`-Wall -Wextra -Wshadow`）、テスト9件×2（通常+ASan/UBSan）全ok
- デモ: `ls /dir2/subdir1`がsubdir1自身の内容を表示（M5解消）、catはバイナリ安全、破損時はSEGV而非ずstderr警告で停止
- 破損イメージ回帰（ASan+UBSan、報告ゼロ）: 0x55AA破壊/rsv=0xFFFF/totalSectors16=0はinitが明確メッセージで拒否、FATサイクル8↔9は走査停止、fileSize水増しは警告+正常終了、`fat_get_cluster_ptr(0/1/範囲外)`はNULL
- 増分ビルド: 変更なし再実行で「Nothing to be done」、`touch fat.h`で依存再コンパイル（`.d`追跡動作）
- 統合時の追加修正: testmain.c 3件（チェーン長カウントのオフバイワン、最終部分クラスタ未読、未初期化`DirectoryEntry`のUB）+ リード作業3件（未使用include除去`fat.c`/`main.c`、`Circular build`警告解消のため未使用`build`エイリアル削除）

残課題（フェーズ0以降または要ユーザー判断）:
- `tags`はgit追跡済みのまま（ignoreは追跡ファイルに効かない）。解除には`git rm --cached tags`+コミットが必要
- `demof12.fat`はウェーブ前にmtools 4.0.48で再生成済み（OEM=MTOO4048）。fixture戦略（凍結+CI diff か untrack+生成依存）は未決定
- FATタイプ閾値`< 4085`化、再init失敗時の旧バッファ残存、`fat_get_fat`の引数範囲検査、0x05頭バサイズ、`'.'`/`'..'`解決 → フェーズ1/2へ繰り越し

## 10. フェーズ0実施結果（2026-10-05 完了）

アーキテクチャ: `fat.h`（公開契約: `fat_ctx_t`不透明・`fat_result_t`・`fat_dirent_t`(`name[256]`)・`fat_geometry_t`）/ `fat_internal.h`（ディスク上構造体・`struct fat_ctx`・ATTR_*、ライブラリ内専用）/ `fat_core.c`（stdio・color・ファイルスコープ変数ゼロ）/ `fat_dev.c`（`fat_open`のみ、stdioはここだけ）/ `fat_dump.c`（`fat_print_*`、`const fat_ctx_t*`）。`fat.c`は削除。

達成: コンテキスト化（複数イメージ・`fat_close(NULL)`安全・コアのグローバル状態ゼロ）、エラーenum+`fat_strerror`（ライブラリ内印刷廃止）、モデル/ビュー分離、`fat_open_mem`（メモリ変異テスト: 0x55AA破壊・FAT自己ループ・fileSize水増し→`BAD_CLUSTER`等、外部ハーネス不要）、`fat_lookup`（`.`/`..`解決、`NOT_FOUND`/`PATH_NOT_FOUND`区別）、`fat_read_file`（バイナリ安全・所有権明確）、FATタイプ閾値のspec準拠化（`< 4085`）、`totalSectors16==0`→`FAT_ERR_UNSUPPORTED`。

検証: `make check` = 9テスト×2（通常+ASan/UBSan）全ok・警告ゼロ。デモ全セクション正常（新規`cat /dir1/../hello.txt`、`test_5kb.txt`4962バイト完全一致、クラスタ直書き廃止）。

未決の契約判断: ルート直下の`.`のlookupは`FAT_ERR_PATH_NOT_FOUND`（`..`と対称・実装側）。DOS流にrootの合成direntで`FAT_OK`にする選択も可（1行変更）。

フェーズ候補のnotes: 公開APIのconst一貫性（`fat_dump.c`のconst外し1箇所）、`fat_print_directory_entry`が実質デッドexport、`-Wstrict-prototypes`対応、dumpの色サイクルstatic（フェーズ5再入化時）。

## 11. フェーズ2実施結果（2026-10-05 完了）

チーム編成: リードが`fat.h`契約を先に拡張（`fat_geometry_t.root_cluster`、`fat_fsinfo_t`+`fat_fsinfo()`、FAT16/32サポート明記）→ p2-lib（`fat_internal.h`/`fat_core.c`/`fat_dump.c`）、p2-test（`testmain.c`/`Makefile`/`.gitignore`）、p2-app（`main.c`改名追従）の並列分担。p2-lib/p2-testはAPI使用制限（429）でレポート送信直前に中断したが、ファイル書き込みは完了していたため、リードが統合検証を直接実施した。

達成:
- **タイプ別FATデコード**: FAT12従来/ FAT16 u16 / FAT32 u32 `& 0x0FFFFFFF`（マスク必須）。エントリ読取り前にタイプ別バイト長でFAT領域内を検証。EOC/bad/reserved閾値もタイプ別（0xFF8系/0xFFF8系/0x0FFFFFF8系）。
- **totalSectors32対応**: totalSectors16==0なら32bit値（従来の`UNSUPPORTED`撤廃）。fatSizeもtableSize16==0ならfatsz32。領域導出は全て64bit中間計算。
- **FAT32ルート=チェーン走査**: `FatExtBS32`実体化（extended_section[0]、common tailは[28]）。`rootClus`検証（2..cluster_count+1）、`geo.root_cluster`に反映、`root_dir_sector/sectors`は0。`fat_iter_dir(FAT_CLUSTER_ROOT)`はタイプで自動ディスパッチ。
- **active FAT選択**: extFlags bit7でミラー無効時、下位4bitのFAT番号を`fat_start_sector`に反映（範囲外は`INVALID_BPB`）。
- **FSInfo**: `fat_fsinfo()`。署名3種（RRaA/rrAa/0xAA550000）検証、不正・不在は0xFFFFFFFF（unknown）で`FAT_OK`、FAT12/16は`UNSUPPORTED`。
- **firstClusterHigh/Low合成**: FAT32は32bit合成、FAT12/16はlowのみ。
- **dump**: `fat_print_fat12`→`fat_print_fat`改名（%03X/%04X/%08X、FAT32は先頭1024エントリ+打ち切り表示）、`fat_print_info`のタイプ対応、FAT32 EBPBダンプ。

フィクスチャ（mtools実物、gitignore、`make fat16`/`make fat32`で生成、`check-all`=生成+check）:
- `demof16.fat` 16MiB: cluster 32481、チェーン3→..→12、FAT[0]=0xFFF8
- `demof32.fat` 33MiB: cluster 66512、**ルートが3クラスタ chain 2→54→55（44エントリ）**、FSInfo free=66454（mdir 34024448B freeと一致検証済み）

検証: `make check` = **19テスト×2**（FAT12 9 + FAT16 4 + FAT32 6、通常+ASan/UBSan）全ok・警告ゼロ。fixture不在時はスキップ表示で`check`は高速維持。デモはdemof12で従来どおり全セクション正常。

統合時にリードが修正したバグ1件: `fat_read_file`のチェーンループ検出は「サイズ不足で飢えるループ」しか捕捉せず、自己ループ（FAT[5]=5）はfile_size分をゴミで埋めて`FAT_OK`になる契約違反 → **Brentのサイクル検出**（O(1)空間、1ステップ1比較）をreadウォークに追加し「loops→BAD_CLUSTER」契約を遵守。

残課題: FAT16/32のOEM・fatsz整合の追加mutation、ルート直下の`.`のlookup挙動（§10未決のまま）、フェーズ3 LFN（0x0F連続エントリ）は現在スキップのみ。

## 12. フェーズ4（書き込み）詳細設計（2026-10-05 追記）

前提: フェーズ0〜2完了（コンテキスト化・エラーenum・FAT12/16/32デコード）。イメージは`fat_open`/`fat_open_mem`で丸ごとRAM載せのため、フェーズ4の書き込みはメモリ上のイメージコピーへの更新とし、永続化は`fat_write()`（全バッファをオリジナルパスへ書き戻し）で行う。I/O抽象化（フェーズ5）でこの境界を`fat_io_t`に差し替える。

### 12.1 FATエントリ書き込み `fat_set_fat_entry`

- 12bitはバイト非アライメント。オフセット`cluster*3/2`の2バイトを既にFAT12では1バイト読みの`fat_get_fat_entry`で処理済みだが、書き込みは必ず**read-modify-write**（隣接エントリとニブルを共有するため、隣を壊さない）:
  - 偶数: `b[0] = value & 0xFF`、`b[1] = (b[1] & 0xF0) | (value >> 8)`
  - 奇数: `b[0] = (b[0] & 0x0F) | (value << 4)`、`b[1] = value >> 8`
- **両FATコピーへ書き込む**: `fat_count`分のテーブル先頭オフセットをループ。ミラー間不一致は書き込み後に一致検証するか、少なくとも既存チェーン読み取りと同じactive FAT選択規則に従う。
- 値はタイプ別に妥当化: EOCマークはタイプ別定数（0xFFF / 0xFFFF / 0x0FFFFFFF）、FREEは0。`cluster`は2..cluster_count+1のみ（0/1はINVALID_ARG）。
- FAT32は既存どおり`& 0x0FFFFFFF`マスク（上位4bitは予約のため保存）。

### 12.2 クラスタ確保 `fat_alloc_cluster`

- FAT[2..]を先頭から走査しFREE（0）を探す。FAT32はFSInfoの`next_free_cluster`ヒントから試み、その値が本当にFREEのときのみ採用（stale=0xFFFFFFFFや誤値は無視して全走査）。
- 見つけたらEOCマークを書き込み、FSInfoのfree数を減算。空きなしは新enum `FAT_ERR_DISK_FULL`。
- 確保後はチェーン接続は呼び出し側の責務（`fat_set_fat_entry(prev, new)`）。

### 12.3 クラスタ解放 `fat_free_chain`

- 先頭からEOCまで辿って各エントリをFREEに。サイクル検出は`fat_read_file`に追加済みのBrent法と同じ上限（cluster_count反復）で防御。
- FSInfoのfree数を加算し、`next_free_cluster`ヒントを先頭クラスタに更新（or unknown化）。

### 12.4 ディレクトリスロット探索 `fat_add_dirent`

- 0x00=未使用（以降も0x00が続くため、ここから連続枠を取れる）、0xE5=削除済み（単独で再利用可）。
- フェーズ4は8.3のみでスロット1個。フェーズ3 LFN結合後は「短名+LFN N個」の連続N+1枠が必要（0x40フラグ・チェックサムはフェーズ3で設計）。
- FAT12/16のルートは固定領域（`root_entries`上限、拡張不可）→ 満杯は新enum `FAT_ERR_DIR_FULL`。
- サブディレクトリとFAT32ルートはチェーン走査し、空き枠がなければ`fat_alloc_cluster`でチェーンを延長し、新クラスタを0x00でゼロ埋め（0x00=終端マーカーを保証）。

### 12.5 FSInfo更新（FAT32のみ）

- 確保/解放に応じて`free_cluster_count`±1、`next_free_cluster`書き換え。署名不正・不在（0xFFFFFFFF）のときは壊さずunknownのまま維持（§2の`fat_fsinfo()`契約と対称）。

### 12.6 DOSタイムスタンプ

- FatDate = `((year-1980)<<9) | (month<<5) | day`、FatTime = `(hour<<11) | (min<<5) | (sec/2)`（2秒粒度、`creation_time_tenth`が追加精度）。
- 作成時: `creation_date/time` + `last_write_date/time` + `last_access_date`（FAT32慣例）。更新時: `last_write_*`のみ。
- タイムゾーンは実装方針を明記（DOS流ローカルタイム or UTC、どちらでもよいが統一）。

### 12.7 書き込み順序（失敗時一貫性）

クラッシュ耐性はRAM載せ+全体flushのため題外だが、**操作途中の失敗で不整合を残さない**順序にする:

1. データ書き込み先クラスタを確保（チェーン構築）→ 2. データ全量を書き込み → 3. 最後にdirentのfirst cluster / file_sizeを更新。

どのステップで失敗しても、確保済みチェーンを`fat_free_chain`で解放して`FAT_ERR_*`を返す。direntが最後に変わるため、失敗時に「direntだけ有効でデータ無し」の孤立参照を作らない。

### 12.8 公開API契約案（`fat.h`追記）

```c
fat_result_t fat_set_fat_entry(fat_ctx_t* ctx, uint32_t cluster, uint32_t value);
fat_result_t fat_alloc_cluster(fat_ctx_t* ctx, uint32_t* out);
fat_result_t fat_free_chain(fat_ctx_t* ctx, uint32_t head);
fat_result_t fat_add_dirent(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const fat_dirent_t* tmpl);
fat_result_t fat_write_file(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const uint8_t* data, size_t size);
                            /* 12.7の順序で alloc→write→dirent更新 */
fat_result_t fat_write(fat_ctx_t* ctx, const char* path); /* 全イメージflush。NULL=open元パス */
```

enum追記: `FAT_ERR_DISK_FULL`（FAT空きなし）、`FAT_ERR_DIR_FULL`（ディレクトリ枠なし）。

### 12.9 テスト計画

- mtoolsオラクル差分: 書き込み後に`mdir -b`/`mtype`で目録・内容一致assert（§5方針の継続）。
- 12bitニブル隣接保護: クラスタ3書き込み後、2/4の値が不変であること（偶数/奇数両方）。
- 両FATコピーの一致、FAT12/16ルート満杯、0xE5再利用、FAT32ルートチェーン延長、DISK_FULL。
- 書き込み→`fat_open`再読み込みでround-trip一致。全ターゲットを`test-san`（ASan/UBSan）に追加。

## 13. ウェーブA&B: 正確性の詰め + ライブラリAPI（2026-10-05 計画、TDD実施）

LFN（フェーズ3）は見送り。読み取り専用ライブラリとしての完成度を上げる。**TDD**: リードが`fat.h`契約を先に拡張（完了）→ テストエージェントが全テストを先行記述しredを確認（この間ライブラリはコミット済みの安定状態）→ 実装エージェントがgreen化 → リードが最終検証。

### 13.1 契約変更（fat.h拡張済み）

- **A2 ルート直下の`.`/`..`**: DOS流に**合成direntで`FAT_OK`**（name="."/".."、ATTR_DIRECTORY、first_cluster=FAT_CLUSTER_ROOT、size=0）。§10の未決を決着。
- **A3 ボリュームラベル非マッチ**: `fat_lookup`はATTR_VOLUME_IDエントリにマッチしない（DOS open()準拠）。
- **A1 0x05頭バイト**: `fat_name_from_83`はname11[0]==0x05を0xE5として描画（日本語名エスケープ）。
- **B6 ストリーミング読み**: `fat_file_t`（opaque）+`fat_file_open/seek/read/tell/size/close`。全丸ごとmallocの`fat_read_file`はFAT16の2GBで破綻するため。seekはO(チェーン長)、readはEOF以外ショートなし、loopは`fat_read_file`と同じBrent防御で`BAD_CLUSTER`。
- **B7 ディレクトリカーソル**: `fat_dir_t`（opaque）+`fat_dir_open/next/close`。終端は新enum **`FAT_ERR_END_OF_DIR`**（継続呼び出しも同値）。`fat_iter_dir`は内部でcursor実装に統合（走査ロジックの二重化を防ぐ）。
- **B8 タイムスタンプ**: `fat_dos_date_to_tm(dos_date, dos_time, dos_tenth, struct tm*)`。範囲外フィールドは`INVALID_ARG`。タイムゾーンは中立（tm_isdst=0）。
- `fat_strerror`にEND_OF_DIR分を追加（リードが基盤として先行投入）。

### 13.2 テスト追加（p-ab-test、testmain.c/Makefile所有）

- A1: 0x05レンダリング単体 + 8.3ラウンドトリップ
- A2: `.`/`..` at root → FAT_OK + 合成dirent検証、`dir1/../..` の追跡
- A3: `"demof12"`/`"demof16"`/`"demof32"` lookup → NOT_FOUND
- B6: 全ファイル読み（`fat_read_file`とのバイト一致）、seek+tell、境界（0/size/size+1）、空ファイル、ショートしないread、FAT32の多クラスタ、突然のBAD_CLUSTER（mutation）
- B7: `fat_iter_dir`との列挙一致（3フィクスチャ）、終端後の継続呼び出し、root/FAT32ルートチェーン
- B8: DOS日時デコード（境界値1980/2107、月0/13、日0、時24、tenth）
- **A4 ブートセクタファズ**: FAT12は512B全bit反転（4096变异、open→iterate→lookup→readの生存）、FAT16/32はBPBフィールド値ベースmutation（{0x00,0xFF,0x55}×主要オフセット。イメージコピー34MB×4096は実行時間非現実的のため）。ASanビルドで実行。
- **A5 mtools差分**: `mdir -b`出力と当方の列挙（名前/サイズ/種別）を3フィクスチャ横断で比較、`mtype`と`fat_read_file`のバイト一致。mtools不在時はスキップ表示。
- Makefile: `-Wstrict-prototypes`追加、ファズ・差分を`check`に含めつつ実行時間を概算1分以内に。

### 13.3 実装（p-ab-lib、fat_internal.h/fat_core.c/fat_dump.c所有）

- A1/A2/A3: `fat_name_from_83`/`fat_lookup`（合成dirent生成、label除外）
- B6: `struct fat_file`（ctx借用+direntコピー+位置/クラスター状態+Brent状態）
- B7: `struct fat_dir`（走査状態）で`fat_iter_dir`を再実装（同一ロジック共用）
- B8: ビット分解デコーダ
- **A9 掃除**: `fat_dump.c`のconst外し解消、`fat_print_directory_entry`デッドexport削除、`-Wstrict-prototypes`対応
- 制約: コアはstdioゼロ・グローバル状態ゼロ・警告ゼロを維持。テストファイルは触らない（テストの誤りはリードにエスカレーション）

### 13.4 検証

redリスト記録 → green化 → リードが`make check`（+ASan）とデモを直接実行、§14に結果記録。コミットは指示待ち。

## 14. ウェーブA&B実施結果（2026-10-05 完了）

TDD実施: **p-ab-test**がred フェーズで全テスト先行記述（A1/A2/A3はアサーションred、B6/B7/B8はリンクred=未定義シンボル10個、A4ファズ/A5 mtools差分は現行libで緑の回帰ロック。red理由はプローブ実測で「契約未実装」であることを全件確認）→ **p-ab-lib**がgreen化（凍結テスト・契約fat.h変更なし、指摘ゼロで一発green）→ リード検証。

達成:
- **A1**: `fat_name_from_83`はname11[0]==0x05を0xE5描画
- **A2**: ルート直下の`.`/`..`は合成dirent（name/ATTR_DIRECTORY/`FAT_CLUSTER_ROOT`/size 0）で`FAT_OK`。§10の未決解消。非ルートの`.`は実ディスク"."エントリ解決（相対開始クラスタ対応）
- **A3**: `fat_lookup`はATTR_VOLUME_ID非マッチ（DOS open()準拠）
- **B6**: `fat_file_t`ストリーミングAPI（open/seek/read/tell/size/close）。seekはO(チェーン)再ウォーク、readは`fat_read_file`と同一ガード（bad/free/reserved/EOC短絡/bounds/Brent）を呼び出し内継続、エラー時カーソル不変
- **B7**: `fat_dir_t`カーソルAPI。`fat_iter_dir`は`fat_dir_open`+nextループのthin実装に統合（走査単一化）。END_OF_DIRはsticky。**非ルートディレクトリクラスタは先頭"."エントリ必須の検証**を共有パスに追加（ファイルデータ誤指定をINVALID_ARGで拒否、FAT32のroot_cluster直指定も免除対象）
- **B8**: `fat_dos_date_to_tm`（範囲検証: 月1-12/日≥1/時≤23/分≤59/秒フィールド≤29/tenth≤199）
- **A4**: FAT12ブートセクタ全bit反転4096変異（ASan込み3.8秒）+ FAT16/32値変異（9/13オフセット×3値）— クラッシュ/ASan報告ゼロ
- **A5**: mtools差分（`mdir -b`列挙集合比較×6 Listing、`mtype`バイト一致×3）— 現行libで一致、回帰ロック化
- **A9**: `-Wstrict-prototypes`対応5箇所（color×2、fat_dump×3）、`fat_print_directory_entry`デッドexportと到達不能静的群（約90行）削除、const外し解消

検証（リード直接実施）: クリーンビルド `make check` = **37テスト×2全ok、警告ゼロ**（`-Wall -Wextra -Wshadow -Wstrict-prototypes`、通常+ASan/UBSan）。デモexit=0・全セクション正常。実行時間はASan込み全体で約4秒（ファズフルスイープ含む）。

テスト構成: FAT12基盤9 + ファズ/mtools回帰 + FAT16 5 + FAT32 8 + 新API群（file 6/dir 5/dos日時/0x05/ラベル/ルートdots）= 37。

次のステップ（§12設計済み）: フェーズ4書き込み対応（`fat_set_fat_entry`/`fat_alloc_cluster`/`fat_free_chain`/`fat_add_dirent`/`fat_write_file`/`fat_write`、FAT12 RMW・両FATコピー・FSInfo更新・DOSタイムスタンプ・失敗時一貫性順序）。

## 15. フェーズ4実施結果（2026-10-06 完了）

TDD実施: リードが`fat.h`に書き込みAPI契約を先行確定（§12.8から`fat_write_file`の`tmpl`引数と`FAT_ERR_EXISTS`を追加）→ **p4-test**が全テスト先行記述しred確認（既存シンボルはアサーションred、新シンボル6個はリンクred+`/tmp`参照スタブハーネスでテスト自己整合性を立証）→ **p4-lib**がgreen化（テスト凍結・契約変更なし）。中断なし、両エージェント完走。

達成（`fat_core.c`書き込みセクション約540行 + `fat_dev.c`の`fat_write`）:
- **`fat_set_fat_entry`**: FAT12はread-modify-write（オフセット`cluster*3/2`、偶数/奇数で隣接ニブル保護）。ミラーは全FATコピーへ、extFlags bit7で無効時はactive FATのみ（テーブルkの基底は`reserved_sectors + k*fat_sectors`から導出）。FAT32は上位4予約ビットを既存値から保存。値上限はタイプ別（0xFFF/0xFFFF/0x0FFFFFFF）。
- **`fat_alloc_cluster`**: FAT32はFSInfoのnext_freeヒントを起点に环形走査（staleは2へフォールバック）。EOCマーク+FSInfo同期。空きなしは`DISK_FULL`（走査は読み取りのみでイメージ不変）。
- **`fat_free_chain`**: 歩いたエントリをその場でゼロ化（zero-as-you-goでサイクルも自然破壊）、broken/loop時は歩いた分だけ解放済みの状態で`BAD_CLUSTER`。
- **`fat_add_dirent`**: ワンパス走査（名前衝突/0xE5/0x00/チェーン尾部を1回で収集）。スロット優先順位は**0xE5→0x00→チェーン延長**。FAT12/16固定ルートは`DIR_FULL`。延長は1クラスタゼロ埋め（0x00終端を保証）。
- **`fat_write_file`**: §12.7のクラッシュ一貫性順序（alloc→データ→dirent最後）、失敗時`fat_free_chain`で完全巻き戻し。EXISTS判定は確保前に実施（失敗書き込みが空きクラスタ数を動かさない）。空ファイルはfirst_cluster 0。
- **`fat_write`**: イメージ全体を`path`へflush（作成/切捨て）。

テスト（54テスト、+1004行）: ニブル隣接保護（偶数/奇数クラスタ書き込み後の隣接エントリ不変）、両ミラーraw一致、FAT32予約ビット保存（0xA1234567）、FSInfo増減、0xE5スロット再利用（バイトオフセット完全一致）、EXISTS-before-alloc（空き数監視）、`DIR_FULL`（rootEntryCount=5変異）、FAT32ルート延長（49エントリ、FSInfo -1）、クラスタ境界1023/1024/1025/2048/2049→1/1/2/2/3クラスタ、ロールバック（空き2+5クラスタ要求→DISK_FULLで完全復元）、round-trip+mtoolsオラクル（`mtype`バイト一致・`mdir`列挙一致）、`fat_write`でflush→再読込。

検証（リード直接実施）: クリーンビルド `make check` = **54テスト×2全ok、警告ゼロ**（`-Wall -Wextra -Wshadow -Wstrict-prototypes`、通常+ASan/UBSan）。デモexit=0。変更ファイルは`fat.h`/`fat_core.c`/`fat_dev.c`/`testmain.c`の4つ。

設計からの修正・逸脱:
- **§12.1誤り（正誤）**: 奇数クラスタRMWの設計式`b[1]=value>>8`は誤り。正しくは`b[0]=(b[0]&0x0F)|(value<<4)`、`b[1]=(value>>4)&0xFF`。さらにp4-libが自己発見した`(cluster/2)*3`は奇数で`floor(cluster*3/2)`と一致しない（隣接ニブル破壊）→ `cluster*3/2`（size_t）に修正。
- **§12.8からの逸脱（3件、全てリード承認済み）**: (1) `fat_write`はpath必須（NULL=open元パスの暗黙状態は廃止） (2) `fat_write_file`に`tmpl`引数追加（時刻ホックより決論的、NULL=ATTR_ARCHIVE+ゼロタイムスタンプ） (3) 新enum `FAT_ERR_EXISTS`（無言上書きより明示拒否）。
- **p4-libが自己修正したバグ2件**: FAT12奇数オフセット（上記）、スロット優先順位（初期実装は0x00優先だったが契約どおり0xE5優先に修正）。
- **リード指示の誤りをテスト側が修正**: DIR_FULLのrootEntryCount変異は6でなく5（5生存エントリ+1空きでは追加成功してしまう）。FAT32ルート延長のFSInfo減算は-6でなく-1（`fat_add_dirent`はデータクラスタを確保しない）。

残課題: overwrite/truncate（既存ファイルの再書き込み）、削除（0xE5マーク+チェーン解放）、`fat_file_t`書き込み側カーソル — いずれも本フェーズのスコープ外（契約明記済み）。
