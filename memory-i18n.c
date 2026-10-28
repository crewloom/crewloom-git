#include "git-compat-util.h"
#include "memory.h"

static const struct memory_message {
	const char *code, *en, *ar;
} messages[] = {
	{ "STORE_MISSING", "Memory is not initialized; select tracked paths with memory init", "الذاكرة غير مفعلة؛ حدد المسارات المتتبعة عند التهيئة" },
	{ "STORE_UNSAFE", "Memory store must be private, owned and free of symbolic links", "مخزن الذاكرة يجب أن يكون خاصا بالمستخدم ودون روابط رمزية" },
	{ "STORE_FOREIGN", "Memory store belongs to another worktree or Git directory", "مخزن الذاكرة يخص شجرة عمل أو مجلد Git آخر" },
	{ "IO", "Memory input or storage operation failed", "فشلت قراءة المدخل أو عملية تخزين الذاكرة" },
	{ "SCOPE", "Select literal tracked paths within this repository", "حدد مسارات متتبعة صريحة داخل المستودع" },
	{ "UNSUPPORTED", "Selected input is unsupported; no snapshot was published", "المدخل المختار غير مدعوم؛ لم تنشر لقطة جديدة" },
	{ "LIMIT", "Selected input exceeds the configured memory limits", "المدخل المختار يتجاوز حدود الذاكرة المحددة" },
	{ "CONFLICT", "Inputs changed during capture; retry after writers finish", "تغيرت المدخلات أثناء الالتقاط؛ أعد المحاولة بعد انتهاء الكتابة" },
	{ "OBJECT", "Memory object is missing, invalid or corrupt", "كائن الذاكرة مفقود أو غير صالح أو تالف" },
	{ "CONFIG", "Memory configuration or environment is unsupported", "إعدادات الذاكرة أو بيئة التشغيل غير مدعومة" },
	{ "OPTIONS", "Invalid memory operation or arguments", "عملية الذاكرة أو معاملاتها غير صالحة" },
	{ "LOCK", "Memory store is busy; retry without removing a live lock", "مخزن الذاكرة مشغول؛ أعد المحاولة ولا تحذف قفلا نشطا" },
	{ "NO_SNAPSHOT", "No recorded memory snapshot exists", "لا توجد لقطة ذاكرة مسجلة" },
	{ "NO_MATCH", "The requested source is outside the recorded snapshot", "المصدر المطلوب خارج اللقطة المسجلة" },
	{ "BUDGET", "Required memory metadata exceeds the output byte budget", "بيانات الذاكرة المطلوبة تتجاوز ميزانية بايتات المخرجات" },
	{ "UTF8", "JSON input must be valid UTF-8 without embedded NUL bytes", "مدخل JSON يجب أن يكون UTF-8 صالحا ودون بايتات NUL داخلية" }
};

int memory_error(struct memory_store *m, struct strbuf *err,
		 const char *code, const char *detail)
{
	size_t i;
	for (i = 0; i < ARRAY_SIZE(messages); i++)
		if (!strcmp(messages[i].code, code))
			break;
	strbuf_reset(err);
	strbuf_addf(err, "%s: %s", code,
		    i < ARRAY_SIZE(messages) ?
		    (m->arabic ? messages[i].ar : messages[i].en) : code);
	if (detail && *detail)
		strbuf_addf(err, " (%s)", detail);
	return -1;
}
