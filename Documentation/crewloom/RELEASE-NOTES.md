# Crewloom Git 0.1.1 — Prototype frozen

Documentation only; no code changes from 0.1.0. On 2026-10-29 the native builtin was
frozen as a reference prototype. New work moves to a change-memory layer on top of an
unmodified, installed Git, gated on measuring token and cost savings on real tasks
first. See `IMPLEMENTATION-PLAN.md`.

CI: the macOS job runs on `macos-15`, because `macos-26` jobs were cancelled
without a runner being assigned.

بالعربية: تحديث للوثائق فقط، دون تغيير في الكود. الأمر الأصلي أصبح نموذجًا أوليًا
مجمدًا، والعمل الجديد طبقة فوق Git المثبت بعد قياس توفير الرموز والتكلفة على مهام حقيقية.

# Crewloom Git 0.1.0 — Native project memory

This release adds a native Git builtin for a private, local, versioned project
memory. The memory store is off until the user explicitly selects tracked paths.
It keeps staged index bytes separate from raw working-tree bytes, captures saved
changes with a foreground watcher, and offers bounded context, history, byte-span
diffs, status, and source-linked untrusted notes. No model call is used for capture,
storage, diffing, or retrieval. This does not replace the system Git executable.

## Scope and known limits

- Built against upstream Git v2.56.0 in the `crewloom-main` fork.
- Model, actor, session and task fields are host declarations, not authenticated
  attribution. Observation time is not the edit time.
- The watcher is foreground polling. Unsaved keystrokes, edits between polls, and
  automatic capture from unrelated Git commands are not covered.
- Context is bounded path/content matching with source-linked excerpts. No
  Tree-sitter, semantic dependency graph, token count, quota-saving result, or
  intelligence improvement is claimed.
- Memory objects live in a separate per-worktree owner-only bare store under the
  Git directory. Repository clone, mirror push and bundle omit it; copying or
  backing up `.git` may include it. Selected historical raw bytes are not erased
  automatically, so exclude secrets from the chosen scope.
- SHA-1/SHA-256 and regular files are supported. Conflicts, sparse/skip-worktree
  entries, gitlinks, partial clones, and environment-redirected object/index stores
  fail explicitly or are unsupported as described in `git-memory(1)`.

## Verification

Executed locally on 2026-10-28 (macOS arm64, default Clang):

- `./scripts/build-memory.sh DEVELOPER=1` — passed; compiled the complete Git
  executable with the native builtin.
- `./scripts/test-memory.sh` — passed: 32 memory tests (23 behavior and 9 safety)
  plus 674 tests across the selected Git compatibility suites (706 total).
- `git diff --check` — passed.

The local run did not include sanitizers or Windows. The release CI matrix must
pass on Ubuntu, macOS, and the Ubuntu address/undefined-behavior sanitizer job;
the release is not complete until those GitHub Actions runs are verified with
`gh`.

## بالعربية

يضيف هذا الإصدار أمرًا أصليًا داخل Git لحفظ تاريخ ذاكرة محلية وخاصة للمسارات
المتتبعة التي يحددها المستخدم. يفصل محتوى الملفات staged عن محتوى شجرة العمل،
ويستطيع مراقب foreground حفظ التغييرات المحفوظة أثناء تشغيله. الاسترجاع والفروق
والتخزين لا تستدعي نموذج ذكاء اصطناعي. الإصدار لا يستبدل Git النظام.

قيم model وactor وsession وtask بيانات يعلنها المضيف وليست إثباتًا للهوية. لا يوجد
Tree-sitter أو رسم اعتماديات دلالي، ولا ندّعي نسبة توفير للكوتا أو زيادة الذكاء.
المخزن داخل مجلد Git لكل worktree، وقد تحتوي نسخة احتياطية من `.git` عليه. لا
تحدد ملفات حساسة في النطاق لأن البايتات التاريخية لا تُمحى تلقائيًا.

نتائج التحقق المحلي: البناء نجح، ونجحت 32 حالة ذاكرة و674 حالة توافق Git مختارة
(706 إجمالًا). CI يبني على Ubuntu وmacOS ويشغّل اختبارات sanitizer ويبني صفحة الدليل؛
لا تشمل المجموعة Windows. راجع [الدليل العربي](README.ar.md) و[صفحة الأمر](../git-memory.adoc).
