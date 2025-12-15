#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <stdarg.h>
#include <signal.h>

#define MAX_TEXT 128

// предложение (валентинка) от поклонника
typedef struct {
    int fan_id;
    int score;                 // "привлекательность" предложения
    char text[MAX_TEXT];       // описание вечера
} Offer;

// ответ студентки каждому поклоннику
typedef struct {
    int accepted;              // 1 = да, 0 = нет
    int winner_id;             // кто победил (для общего сведения)
    int best_score;            // лучший балл (для общего сведения)
} Reply;

static int gN = 0;             // количество поклонников
static Offer *gOffers = NULL;  // предложения от всех поклонников
static Reply *gReplies = NULL; // ответы студентки всем поклонникам

// флаги для активного ожидания
static atomic_int *gSubmitted = NULL;  // предложение отправлено
static atomic_int *gReplied   = NULL;  // ответ получен

// итоговые значения
static atomic_int gWinnerId  = -1;     // победитель
static atomic_int gBestScore = -1;     // лучший score

// флаг завершения по Ctrl+C
static atomic_int gStop = 0;

// мьютекс только для печати
static pthread_mutex_t gPrintLock = PTHREAD_MUTEX_INITIALIZER;


// обработка ошибок pthread
static void die_pthread(int rc, const char *where) {
    if (rc == 0) return;
    fprintf(stderr, "pthread error at %s: %s\n", where, strerror(rc));
    exit(1);
}

// обработка системных ошибок
static void die_errno(const char *where) {
    fprintf(stderr, "error at %s: %s\n", where, strerror(errno));
    exit(1);
}

// потокобезопасная печать
static void safe_print(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&gPrintLock);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&gPrintLock);
}

// rand_r в диапазоне [lo..hi]
static int rand_between(unsigned *seed, int lo, int hi) {
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    return lo + (int)(rand_r(seed) % (unsigned)(hi - lo + 1));
}

// обработчик SIGINT
static void on_sigint(int signo) {
    (void)signo;
    atomic_store(&gStop, 1);
}

// аргументы потока-поклонника
typedef struct {
    int fan_id;
    unsigned base_seed;
} FanArgs;


// поток поклонника
static void *fan_thread(void *arg) {
    FanArgs *a = (FanArgs*)arg;
    int id = a->fan_id;

    // индивидуальный seed
    unsigned seed = a->base_seed ^ (unsigned)(id * 2654435761u);

    // имитация "раздумий"
    int think = rand_between(&seed, 1, 3);
    for (int i = 0; i < think; ++i) {
        if (atomic_load(&gStop)) {
            safe_print("[Клиент %02d] Прервано (SIGINT) во время обдумывания.\n", id);
            return NULL;
        }
        sleep(1);
    }

    // формируем предложение
    Offer offer;
    offer.fan_id = id;
    offer.score  = rand_between(&seed, 1, 100);

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

    snprintf(offer.text, sizeof(offer.text),
             "%s", ideas[rand_between(&seed, 0, 7)]);

    // отправка предложения
    gOffers[id] = offer;
    atomic_store(&gSubmitted[id], 1);

    safe_print("[Клиент %02d] Отправил валентинку: score=%d, идея='%s' (думал %dс)\n",
               id, offer.score, offer.text, think);

    // ожидание ответа
    while (!atomic_load(&gReplied[id])) {
        if (atomic_load(&gStop)) {
            safe_print("[Клиент %02d] Прервано (SIGINT) во время ожидания ответа.\n", id);
            return NULL;
        }
        sched_yield();
    }

    // обработка ответа
    Reply rep = gReplies[id];
    if (rep.accepted) {
        safe_print("[Клиент %02d] Ответ: Принято! (best_score=%d)\n", id, rep.best_score);
    } else if (rep.winner_id < 0) {
        safe_print("[Клиент %02d] Ответ: Отказ. (работа остановлена пользователем)\n", id);
    } else {
        safe_print("[Клиент %02d] Ответ: Отказ. Победил %02d (best_score=%d). Реакция: '%s'\n",
                   id, rep.winner_id, rep.best_score,
                   (offer.score + 10 < rep.best_score) ?
                   "надо было стараться(" : "обидно, почти выиграл!");
    }

    return NULL;
}


// рассылка отказов при аварийном завершении
static void send_abort_replies(void) {
    for (int i = 0; i < gN; ++i) {
        gReplies[i].accepted   = 0;
        gReplies[i].winner_id = -1;
        gReplies[i].best_score = -1;
        atomic_store(&gReplied[i], 1);
    }
}


// поток студентки (сервер)
static void *girl_thread(void *arg) {
    (void)arg;

    safe_print("[Сервер] Студентка: жду все валентинки...\n");

    // ожидание всех предложений
    for (;;) {
        if (atomic_load(&gStop)) {
            safe_print("[Сервер] Получен SIGINT. Рассылаю всем отказ и завершаю.\n");
            send_abort_replies();
            return NULL;
        }

        int ready = 1;
        for (int i = 0; i < gN; ++i)
            if (!atomic_load(&gSubmitted[i])) ready = 0;

        if (ready) break;
        sched_yield();
    }

    safe_print("[Сервер] Все валентинки получены. Выбираю лучшее предложение...\n");

    // выбор максимального score
    int best_id = 0;
    int best_score = gOffers[0].score;
    for (int i = 1; i < gN; ++i)
        if (gOffers[i].score > best_score) {
            best_score = gOffers[i].score;
            best_id = i;
        }

    atomic_store(&gWinnerId, best_id);
    atomic_store(&gBestScore, best_score);

    sleep(1); // пауза для наглядности

    safe_print("[Сервер] Выбрано предложение клиента %02d: score=%d, идея='%s'\n",
               best_id, best_score, gOffers[best_id].text);

    // рассылка ответов
    for (int i = 0; i < gN; ++i) {
        gReplies[i].accepted   = (i == best_id);
        gReplies[i].winner_id = best_id;
        gReplies[i].best_score = best_score;
        atomic_store(&gReplied[i], 1);
    }

    safe_print("[Сервер] Ответы разосланы всем. Завершаю работу.\n");
    return NULL;
}


// разбор целого числа
static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s[0] || (end && *end)) return 0;
    if (v < -2147483647L || v > 2147483647L) return 0;
    *out = (int)v;
    return 1;
}


int main(int argc, char **argv) {
    // формат запуска: ./main N [SEED]
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s N [SEED]\n", argv[0]);
        return 1;
    }

    if (!parse_int(argv[1], &gN) || gN < 1 || gN > 1000) {
        fprintf(stderr, "N must be integer in [1..1000]\n");
        return 1;
    }

    unsigned base_seed = (unsigned)time(NULL);
    if (argc == 3) {
        int seed_i;
        if (!parse_int(argv[2], &seed_i)) return 1;
        base_seed = (unsigned)seed_i;
    }

    // обработчик Ctrl+C
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    // выделение памяти
    gOffers    = calloc(gN, sizeof(Offer));
    gReplies   = calloc(gN, sizeof(Reply));
    gSubmitted = calloc(gN, sizeof(atomic_int));
    gReplied   = calloc(gN, sizeof(atomic_int));
    if (!gOffers || !gReplies || !gSubmitted || !gReplied) die_errno("calloc");

    for (int i = 0; i < gN; ++i) {
        atomic_init(&gSubmitted[i], 0);
        atomic_init(&gReplied[i], 0);
    }
    atomic_init(&gStop, 0);

    safe_print("[MAIN] Старт: N=%d, SEED=%u (Ctrl+C для прерывания)\n", gN, base_seed);

    // запуск сервера
    pthread_t server;
    pthread_create(&server, NULL, girl_thread, NULL);

    // запуск клиентов
    pthread_t *clients = calloc(gN, sizeof(pthread_t));
    FanArgs   *args    = calloc(gN, sizeof(FanArgs));

    for (int i = 0; i < gN; ++i) {
        args[i].fan_id = i;
        args[i].base_seed = base_seed;
        pthread_create(&clients[i], NULL, fan_thread, &args[i]);
    }

    for (int i = 0; i < gN; ++i) pthread_join(clients[i], NULL);
    pthread_join(server, NULL);

    if (atomic_load(&gStop)) {
        safe_print("[MAIN] Завершение по SIGINT.\n");
    } else {
        safe_print("[MAIN] Итог: победил клиент %02d, best_score=%d\n",
                   atomic_load(&gWinnerId), atomic_load(&gBestScore));
    }

    // очистка ресурсов
    free(args);
    free(clients);
    free(gReplied);
    free(gSubmitted);
    free(gReplies);
    free(gOffers);

    return 0;
}