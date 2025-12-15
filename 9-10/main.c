#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>

#define MAX_TEXT 128

// предложение поклонника
typedef struct {
    int fan_id;
    int score;                 // "привлекательность" предложения
    char text[MAX_TEXT];       // описание вечера
} Offer;

// ответ студентки
typedef struct {
    int accepted;              // 1 = да, 0 = нет
    int winner_id;             // кто победил
    int best_score;            // лучший балл
} Reply;


static int gN = 0;             // количество поклонников
static Offer *gOffers = NULL;  // массив предложений
static Reply *gReplies = NULL; // массив ответов

/*
 * В отличие от версии на 8 баллов, здесь НЕТ активного ожидания.
 * Используются условные переменные:
 *
 *  - gAllSubmitted  — все поклонники отправили предложения
 *  - gRepliesReady  — студентка разослала ответы
 */

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  gAllSubmitted = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  gRepliesReady = PTHREAD_COND_INITIALIZER;

static int submitted_cnt = 0;  // сколько поклонников отправили валентинки
static int replies_ready = 0;  // ответы готовы

static int gWinnerId  = -1;
static int gBestScore = -1;
static int gStop      = 0;     // флаг завершения по SIGINT


// мьютекс для синхронизации печати
static pthread_mutex_t gPrintLock = PTHREAD_MUTEX_INITIALIZER;

// файл для логирования (8+ баллов)
static FILE *gLogFile = NULL;

static void die_errno(const char *where) {
    fprintf(stderr, "error at %s: %s\n", where, strerror(errno));
    exit(1);
}

static void die_pthread(int rc, const char *where) {
    if (rc == 0) return;
    fprintf(stderr, "pthread error at %s: %s\n", where, strerror(rc));
    exit(1);
}

/*
 * Безопасный вывод:
 *  - в консоль
 *  - в лог-файл (если задан)
 */
static void safe_print(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&gPrintLock);

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (gLogFile) {
        va_start(ap, fmt);
        vfprintf(gLogFile, fmt, ap);
        va_end(ap);
        fflush(gLogFile);
    }

    fflush(stdout);
    pthread_mutex_unlock(&gPrintLock);
}

// генерация случайного числа в диапазоне
static int rand_between(unsigned *seed, int lo, int hi) {
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    return lo + (int)(rand_r(seed) % (unsigned)(hi - lo + 1));
}

/*
 * Обработчик Ctrl+C:
 * просто выставляем флаг и будим все ожидающие потоки
 */
static void on_sigint(int signo) {
    (void)signo;
    pthread_mutex_lock(&gLock);
    gStop = 1;
    pthread_cond_broadcast(&gAllSubmitted);
    pthread_cond_broadcast(&gRepliesReady);
    pthread_mutex_unlock(&gLock);
}

typedef struct {
    int fan_id;
    unsigned seed;
} FanArgs;

static void *fan_thread(void *arg) {
    FanArgs *a = (FanArgs*)arg;
    int id = a->fan_id;
    unsigned seed = a->seed;

    // имитация "размышлений"
    int think = rand_between(&seed, 1, 3);
    for (int i = 0; i < think; ++i) {
        sleep(1);
        if (gStop) {
            safe_print("[Клиент %02d] Прервано (SIGINT) во время обдумывания.\n", id);
            return NULL;
        }
    }

    // формируем предложение
    Offer offer;
    offer.fan_id = id;
    offer.score = rand_between(&seed, 1, 100);

    const char *ideas[] = {
        "прогулка по городу + кофе",
        "кино + пицца",
        "ужин при свечах",
        "каток + горячий шоколад",
        "настолки + чай",
        "пикник (если погода позволит)",
        "музей + прогулка",
        "концерт + поздний ужин"
    };
    snprintf(offer.text, sizeof(offer.text), "%s",
             ideas[rand_between(&seed, 0, 7)]);

    pthread_mutex_lock(&gLock);

    // отправляем валентинку
    gOffers[id] = offer;
    submitted_cnt++;

    safe_print("[Клиент %02d] Отправил валентинку: score=%d, идея='%s' (думал %dс)\n",
               id, offer.score, offer.text, think);

    // если это последний поклонник — будим студентку
    if (submitted_cnt == gN)
        pthread_cond_signal(&gAllSubmitted);

    // ждём ответа студентки
    while (!replies_ready && !gStop)
        pthread_cond_wait(&gRepliesReady, &gLock);

    Reply rep = gReplies[id];
    pthread_mutex_unlock(&gLock);

    if (gStop) {
        safe_print("[Клиент %02d] Ответ: Отказ. (работа остановлена пользователем)\n", id);
        return NULL;
    }

    if (rep.accepted) {
        safe_print("[Клиент %02d] Ответ: Принято! (best_score=%d)\n",
                   id, rep.best_score);
    } else {
        safe_print("[Клиент %02d] Ответ: Отказ. Победил %02d (best_score=%d)\n",
                   id, rep.winner_id, rep.best_score);
    }

    return NULL;
}

static void *girl_thread(void *arg) {
    (void)arg;

    pthread_mutex_lock(&gLock);
    safe_print("[Сервер] Студентка: жду все валентинки...\n");

    // ждём, пока все поклонники отправят предложения
    while (submitted_cnt < gN && !gStop)
        pthread_cond_wait(&gAllSubmitted, &gLock);

    // если пришёл SIGINT — рассылаем отказ
    if (gStop) {
        for (int i = 0; i < gN; ++i) {
            gReplies[i].accepted = 0;
            gReplies[i].winner_id = -1;
            gReplies[i].best_score = -1;
        }
        replies_ready = 1;
        pthread_cond_broadcast(&gRepliesReady);
        pthread_mutex_unlock(&gLock);
        return NULL;
    }

    // выбор лучшего предложения
    int best_id = 0;
    int best_score = gOffers[0].score;
    for (int i = 1; i < gN; ++i) {
        if (gOffers[i].score > best_score) {
            best_score = gOffers[i].score;
            best_id = i;
        }
    }

    gWinnerId = best_id;
    gBestScore = best_score;

    safe_print("[Сервер] Выбрано предложение клиента %02d: score=%d, идея='%s'\n",
               best_id, best_score, gOffers[best_id].text);

    // рассылка ответов
    for (int i = 0; i < gN; ++i) {
        gReplies[i].accepted = (i == best_id);
        gReplies[i].winner_id = best_id;
        gReplies[i].best_score = best_score;
    }

    replies_ready = 1;
    pthread_cond_broadcast(&gRepliesReady);
    pthread_mutex_unlock(&gLock);

    safe_print("[Сервер] Ответы разосланы всем. Завершаю работу.\n");
    return NULL;
}

static void read_config(const char *fname, int *N, unsigned *seed) {
    FILE *f = fopen(fname, "r");
    if (!f) die_errno("fopen(config)");

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "N=%d", N) == 1) continue;
        if (sscanf(line, "SEED=%u", seed) == 1) continue;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    int N = -1;
    unsigned seed = (unsigned)time(NULL);
    const char *cfg = NULL;
    const char *out = NULL;

    // разбор аргументов командной строки
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-n") && i+1 < argc) N = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i+1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "-o") && i+1 < argc) out = argv[++i];
    }

    if (cfg) read_config(cfg, &N, &seed);
    if (N < 1 || N > 1000) {
        fprintf(stderr, "Invalid N\n");
        return 1;
    }

    gN = N;

    if (out) {
        gLogFile = fopen(out, "w");
        if (!gLogFile) die_errno("fopen(output)");
    }

    // обработка SIGINT
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    gOffers = calloc(gN, sizeof(Offer));
    gReplies = calloc(gN, sizeof(Reply));

    pthread_t server;
    pthread_create(&server, NULL, girl_thread, NULL);

    pthread_t *clients = calloc(gN, sizeof(pthread_t));
    FanArgs *args = calloc(gN, sizeof(FanArgs));

    for (int i = 0; i < gN; ++i) {
        args[i].fan_id = i;
        args[i].seed = seed ^ (unsigned)(i * 2654435761u);
        pthread_create(&clients[i], NULL, fan_thread, &args[i]);
    }

    for (int i = 0; i < gN; ++i)
        pthread_join(clients[i], NULL);
    pthread_join(server, NULL);

    if (gStop)
        safe_print("[MAIN] Завершение по SIGINT.\n");
    else
        safe_print("[MAIN] Итог: победил клиент %02d, best_score=%d\n",
                   gWinnerId, gBestScore);

    if (gLogFile) fclose(gLogFile);
    return 0;
}