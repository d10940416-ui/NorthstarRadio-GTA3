# NorthstarRadio — GTA III
## Сборка через GitHub (без установки компилятора)

---

### Шаг 1 — Создать репозиторий на GitHub

1. Зайдите на **github.com** → войдите в аккаунт (или зарегистрируйтесь, это бесплатно)
2. Нажмите зелёную кнопку **New** → придумайте имя (например `NorthstarRadio-GTA3`)
3. Поставьте галочку **Public** → нажмите **Create repository**

---

### Шаг 2 — Собрать файлы в нужную структуру папок

Создайте на своём компьютере папку с такой структурой:

```
NorthstarRadio-GTA3/
│
├── .github/
│   └── workflows/
│       └── build.yml          ← файл из этого набора
│
├── src/
│   ├── Main.cpp               ← файл из этого набора
│   ├── Switch.cpp             ← файл из этого набора
│   ├── AmbientCar.cpp         ← файл из этого набора
│   └── RadioVehicles.cpp      ← файл из этого набора
│
├── libs/
│   └── bass/
│       ├── bass.h             ← скачать (см. Шаг 3)
│       ├── bass.lib           ← скачать (см. Шаг 3)
│       ├── bass_fx.h          ← скачать (см. Шаг 3)
│       └── bass_fx.lib        ← скачать (см. Шаг 3)
│
├── CMakeLists.txt             ← файл из этого набора
└── NorthstarRadio.ini         ← файл из этого набора
```

---

### Шаг 3 — Добавить библиотеку BASS (обязательно!)

BASS нельзя скачать автоматически — нужно сделать вручную:

1. Откройте **https://www.un4seen.com/** → раздел **Downloads**
2. Скачайте **BASS 2.4** (Windows) → распакуйте архив  
   Из папки `c/` возьмите файлы: `bass.h` и `bass.lib`
3. Скачайте **BASS_FX 2.4** (Windows) → распакуйте архив  
   Из папки `c/` возьмите файлы: `bass_fx.h` и `bass_fx.lib`
4. Положите все 4 файла в папку `libs/bass/`

---

### Шаг 4 — Загрузить файлы на GitHub

**Вариант А — через браузер (самый простой):**
1. Откройте ваш репозиторий на github.com
2. Нажмите **Add file → Upload files**
3. Перетащите все файлы и папки → нажмите **Commit changes**

**Вариант Б — через GitHub Desktop:**
1. Скачайте **GitHub Desktop** (desktop.github.com)
2. Клонируйте ваш репозиторий: **File → Clone repository**
3. Скопируйте все файлы в папку репозитория на компьютере
4. В GitHub Desktop нажмите **Commit to main** → **Push origin**

---

### Шаг 5 — Запустить сборку

Сборка запускается **автоматически** при каждом push файлов.

Или вручную:
1. Откройте ваш репозиторий → вкладка **Actions**
2. Слева выберите **Build NorthstarRadio (GTA III)**
3. Нажмите **Run workflow** → **Run workflow** (зелёная кнопка)

Сборка занимает около **3–5 минут**.

---

### Шаг 6 — Скачать готовый ASI файл

1. Вкладка **Actions** → нажмите на последний запуск (зелёная галочка ✓)
2. Прокрутите страницу вниз до раздела **Artifacts**
3. Нажмите **NorthstarRadio-GTA3** → скачается ZIP архив
4. Распакуйте — внутри будет `NorthstarRadio.asi`

---

### Шаг 7 — Установить мод в GTA III

Скопируйте в папку с игрой:
```
GTA3/
└── scripts/
    ├── NorthstarRadio.asi    ← скачанный файл
    ├── NorthstarRadio.ini    ← файл настроек
    └── ASI Loader ...        ← если ещё не установлен (см. ниже)
```

**Если папки `scripts/` нет** — установите ASI Loader:
- Скачайте **Silent's ASI Loader** или **Ultimate ASI Loader**
  с gtaforums.com или github.com/ThirteenAG/Ultimate-ASI-Loader
- Распакуйте в корень папки GTA III

---

### Что делать если сборка упала с ошибкой

1. Вкладка **Actions** → нажмите на запуск с красным крестиком ✗
2. Нажмите на упавший шаг — будет показан точный текст ошибки
3. Скопируйте текст ошибки и спросите у Claude — поможем разобраться

Самая частая причина: не добавлены файлы BASS в папку `libs/bass/`

---

### Заполнить адреса GTA3_TODO (нужно до первого запуска)

Перед компиляцией откройте `src/Switch.cpp` и найдите строки:
```cpp
#define GTA3_ADDR_ChangeStationJustDown   ((void*)0x00000000) // GTA3_TODO
#define GTA3_ADDR_SetRadioInCar           ((void*)0x00000000) // GTA3_TODO
#define GTA3_ADDR_VehicleRadioProcess     ((void*)0x00000000) // GTA3_TODO
#define GTA3_ADDR_ProcessOneCommand       ((void*)0x00000000) // GTA3_TODO
```

Пока они равны `0x00000000` — радио не будет работать (хуки не применятся).
Адреса можно найти в проекте **re3**: https://github.com/halpz/re3
