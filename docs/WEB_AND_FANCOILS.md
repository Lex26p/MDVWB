# MDVWB: веб-интерфейсы и управление фанкойлами

> Документ описывает текущие статические веб-приложения MDVWB 1.2.0.
> Конфигурация шин, dashboard и расписаний сохраняется только через `mdvwb-manager`; управление фанкойлами выполняется MQTT-командами и подтверждается фактическими состояниями драйвера.

## 1. Веб-приложения

После установки доступны две независимые страницы.

### 1.1. Управление системой и редактор панелей

```text
http://<адрес-WB>/mdvwb/
```

Назначение:

- настройка RS-485-шин;
- start/stop/restart сервисов;
- discovery;
- редактор пользовательских панелей;
- загрузка подложек;
- размещение фанкойлов;
- просмотр revision и reference issues.

Разделы страницы:

```text
/mdvwb/#buses
/mdvwb/#dashboard
```

### 1.2. Пользовательское управление фанкойлами

```text
http://<адрес-WB>/fancoils/
```

Назначение:

- отображение фанкойлов на схеме;
- factual state;
- индивидуальное управление;
- групповое управление;
- расписания.

Конкретная пользовательская panel:

```text
http://<адрес-WB>/fancoils/?panel=floor-2
```

## 2. Требования браузера

Страницы являются статическими HTML/CSS/JavaScript-приложениями.

Внешняя сборка и CDN не требуются.

Браузер должен поддерживать:

- ES modules;
- WebSocket;
- `TextEncoder` и `TextDecoder`;
- `File`, `Blob` и `ArrayBuffer`;
- `ResizeObserver`;
- pointer events;
- современный CSS.

Для загрузки изображений желательно наличие Web Crypto. При его отсутствии используется встроенная реализация SHA-256.

## 3. MQTT WebSocket

Обе страницы автоматически подключаются к same-origin endpoint:

```text
/mqtt
```

Схема выбирается по протоколу страницы:

```text
http  → ws
https → wss
```

Итог:

```text
ws://<host>/mqtt
wss://<host>/mqtt
```

Параметры web client:

```text
MQTT 3.1.1
QoS 0
keepalive 30 seconds
reconnect delay 2 seconds
```

Subscriptions восстанавливаются после reconnect.

Web client не имеет offline command queue. Попытка publish без соединения завершается ошибкой.

## 4. Демонстрационный режим

Обе страницы поддерживают:

```text
?demo=1
```

Примеры:

```text
/mdvwb/?demo=1
/fancoils/?demo=1
```

Demo:

- не требует MQTT;
- не изменяет controller;
- показывает пример нескольких шин и panels;
- имитирует factual states и подтверждение команд;
- подходит для просмотра интерфейса на компьютере.

## 5. `/mdvwb/`: конфигурация шин

Раздел `#buses` показывает:

- число шин;
- число enabled-шин;
- число active service instances;
- manager status;
- port;
- addresses;
- enabled;
- autostart;
- service state;
- discovery state и result.

Редактирование выполняется через draft.

Пока draft отличается от retained `/mdvwb/config`:

- start/stop/restart/discovery отключены;
- можно продолжать редактирование;
- можно отменить draft;
- можно отправить весь JSON на сохранение.

Save использует revision из полученной server configuration.

## 6. Lifecycle шины

Для настроенной шины доступны:

```text
start
stop
restart
status
discovery
```

Start и restart запрещены web UI для:

```json
"enabled": false
```

Stop разрешён независимо от enabled, если manager доступен.

После отправки lifecycle-команды кнопки шины блокируются до result/status.

## 7. Discovery в web UI

Discovery:

- запускается для конкретной настроенной шины;
- останавливает её active service;
- показывает состояние `running`;
- после завершения показывает найденные addresses;
- не меняет draft;
- не записывает addresses автоматически;
- не запускает service обратно.

Найденные адреса пользователь переносит в конфигурацию вручную и сохраняет её отдельной операцией.

Разные шины могут сканироваться одновременно.

Повторный discovery той же шины отклоняется.

## 8. Переход в редактор dashboard

Откройте:

```text
/mdvwb/#dashboard
```

Editor получает retained:

```text
/mdvwb/dashboard/config
/mdvwb/dashboard/status
```

И сохраняет через:

```text
/mdvwb/dashboard/config/set
```

В header показываются:

- dashboard state;
- current revision;
- количество placements;
- количество reference issues;
- признак несохранённых изменений.

## 9. Несколько пользовательских panels

Dashboard version 2 содержит до 64 panels.

Каждая panel имеет:

- ID;
- title;
- собственную подложку;
- собственный список placements;
- собственные пользовательские номера;
- прямую ссылку `/fancoils/?panel=<id>`.

Editor позволяет:

- создать panel;
- сделать копию;
- переименовать ID;
- выбрать panel по умолчанию;
- удалить panel;
- переключаться между panels.

Единственную panel удалить нельзя.

## 10. Panel ID

Ограничения:

```text
1..48 bytes
A-Z a-z 0-9 _ -
```

Примеры:

```text
main
floor-2
office_west
```

Panel ID используется в URL и schedule references.

Изменение ID не является только визуальным rename: ссылки расписаний на старый ID могут стать stale. После сохранения проверяйте scheduler warning.

## 11. Default panel

`defaultPanel` открывается, когда URL не содержит `panel`.

Пример:

```text
/fancoils/
```

Если запрошенная panel отсутствует:

```text
/fancoils/?panel=old-name
```

приложение открывает default panel и показывает warning.

При удалении текущей default panel editor назначает первой оставшейся panel.

## 12. Настройки подложки

Для каждой panel отдельно задаются:

```text
fit
defaultScale
```

Режимы:

| Режим | Поведение |
|---|---|
| Вписать в окно | Подложка помещается целиком |
| По ширине | Подложка занимает доступную ширину |
| Фактический размер | Масштаб 100% |
| Пользовательский масштаб | Используется сохранённый scale |

Допустимый scale:

```text
25%..400%
```

Колесо мыши в editor меняет только preview scale. Сохранённое значение изменяется slider настройки.

## 13. Выбор изображения

Поддерживаются:

```text
PNG
JPEG
WebP
```

Ограничения:

```text
размер файла: 1 byte..10 MiB
размер изображения: 1×1..8192×8192
```

Файл можно:

- выбрать через dialog;
- перетащить в drop zone.

До отправки browser показывает:

- имя;
- размер;
- dimensions;
- preview.

SVG не поддерживается.

## 14. Условия начала upload

Кнопка загрузки доступна, только если:

- MQTT connected;
- dashboard уже получен;
- file выбран;
- нет другого upload;
- нет pending dashboard save;
- settings и placements сохранены либо отменены.

Сначала сохраните изменения panel, затем загружайте background.

Так start и finish работают относительно одной dashboard revision.

## 15. Как передаётся изображение

Browser:

1. вычисляет SHA-256;
2. отправляет metadata start;
3. ждёт подтверждение manager;
4. режет файл на chunks до 48 KiB;
5. отправляет chunk `0`;
6. ждёт progress;
7. отправляет следующий chunk;
8. после полного файла отправляет finish;
9. ждёт `saved=true`.

Raw binary передаётся напрямую по MQTT WebSocket.

Base64 не используется.

## 16. Проверка изображения на controller

Manager независимо проверяет:

- declared size;
- полный SHA-256;
- binary header;
- реальный format;
- extension;
- dimensions.

Переименование произвольного файла в `.png` не позволит его сохранить.

Production filename строится из content hash:

```text
background-<16 hex>.<ext>
```

Assets доступны:

```text
/var/www/fancoils/assets
/fancoils/assets/<file>
```

## 17. Progress и cancel

Во время upload показываются:

```text
received bytes
total bytes
progress %
message
```

Cancel:

- прекращает browser wait;
- отправляет cancel manager;
- удаляет temporary upload;
- не меняет текущую подложку.

При потере MQTT upload считается неуспешным.

## 18. Конкурентная загрузка

Manager поддерживает одну активную upload-сессию.

Не запускайте загрузку одновременно:

- в двух browser;
- на двух panels;
- из web UI и внешнего MQTT client.

Новый start заменяет предыдущую session.

## 19. Конфликт revision во время upload

Если после start другой browser сохранил dashboard:

```text
revision N → N+1
```

то finish старого upload отклоняется.

При этом:

- новая конфигурация N+1 сохраняется;
- background не меняется;
- temporary file удаляется;
- browser получает current revision;
- current dashboard публикуется повторно.

Повторите загрузку после получения новой версии.

## 20. Размещение фанкойлов

Editor строит список устройств из `buses.json`.

Устройство идентифицируется:

```text
Fan-<bus>_<address>
```

Пример:

```text
Fan-2_18
```

Добавление placement не изменяет физический адрес и MQTT topic.

Оно создаёт пользовательское представление на выбранной panel.

## 21. Пользовательский номер

Каждый placement имеет:

```text
number = 1..200
```

Это номер, который видит пользователь.

Он не обязан совпадать с:

- bus;
- MDV address;
- частью MQTT device name.

Пример:

```text
Fan-1_2 → Фанкойл №5
```

Номер уникален внутри panel.

На другой panel тот же номер разрешён.

## 22. Label

Label:

```text
1..120 bytes
```

Используется:

- на marker;
- в tooltip;
- в details drawer;
- в списках group и schedules.

Label не влияет на MQTT topic.

## 23. Visible и удалить

Снятие флажка устройства:

```text
visible = false
```

Marker исчезает с пользовательской panel, но placement остаётся сохранённым.

Команда «Полностью удалить» удаляет placement из dashboard.

Hidden placement продолжает занимать свой пользовательский номер.

## 24. Координаты

Координаты хранятся в долях:

```text
0..1
```

Editor показывает их в процентах.

Перемещение:

- drag мышью или touch;
- Arrow keys — 1%;
- Shift + Arrow — 5%;
- ручной ввод X/Y;
- кнопка центрирования.

Grid можно включать и выключать.

## 25. Размер marker

Допустимый размер:

```text
50%..300%
```

Текущий editor использует общий размер для всех markers выбранной panel.

Backend хранит `markerScale` в каждом placement отдельно, но editor при сохранении выравнивает значения.

## 26. Rotation

Backend schema допускает:

```text
-180..180
```

Текущий web editor не имеет поля rotation и при нормализации сохраняет:

```text
rotation = 0
```

Не редактируйте non-zero rotation вручную, если затем планируется сохранение через текущий editor.

## 27. Reference issues

Issue возникает, когда placement указывает на:

- отсутствующую шину;
- отсутствующий address.

Dashboard остаётся доступен для редактирования.

Такой marker можно:

- удалить;
- скрыть;
- исправить после возврата bus/address.

Изменение `buses.json`, которое создаёт новую broken reference, manager отклоняет.

## 28. Сохранение dashboard

Save отправляет всю collection version 2.

Перед отправкой browser проверяет:

- panel count;
- ID;
- default panel;
- title;
- background;
- placement fields;
- duplicate IDs;
- duplicate numbers;
- duplicate devices.

После успеха revision увеличивается.

## 29. Работа нескольких browser

Каждый editor сохраняет draft от полученной revision.

Если другой browser сохранил раньше:

- manager отклоняет stale save;
- возвращает current revision;
- повторно публикует current dashboard;
- локальный dirty draft не уничтожается;
- editor предлагает отменить draft или сохранить повторно.

Перед повторным сохранением проверьте, что локальный draft не отменяет нужные изменения другого пользователя.

## 30. `/fancoils/`: выбор panel

Обычный URL:

```text
/fancoils/
```

Прямая ссылка:

```text
/fancoils/?panel=main
/fancoils/?panel=floor-2
```

Пользовательская страница не редактирует dashboard.

Она только читает retained configuration и factual states.

## 31. Summary

Для visible markers показываются counters:

```text
Размещено
Видимо
В сети
Аварии
Нет связи
Ожидание
```

Различия:

- `Размещено` — все placements panel;
- `Видимо` — только `visible=true`;
- `В сети` — есть state и устройство не offline;
- `Аварии` — Alarm/Status сообщает ошибку;
- `Нет связи` — `Alarm=2` или `Status=7`;
- `Ожидание` — factual Status ещё не получен.

## 32. Marker state

Marker отображает пользовательский номер и состояние.

Основные Status:

| Value | Состояние |
|---:|---|
| `0` | Выключен |
| `1` | Охлаждение |
| `2` | Нагрев |
| `3` | Осушение |
| `4` | Вентиляция |
| `5` | Автоматический режим |
| `6` | Авария |
| `7` | Нет связи |

Filters позволяют оставить:

```text
все
online
alarm
offline
waiting
```

## 33. Zoom и перемещение карты

Доступны:

- zoom `+` и `-`;
- колесо мыши с центром в позиции pointer;
- drag пустого участка карты;
- кнопка возврата к configured fit;
- автоматический пересчёт fit при resize viewport.

После ручного zoom страница перестаёт следовать configured fit до нажатия fit.

## 34. Details drawer

По нажатию marker открывается карточка:

```text
Device
Label
Status
Room Temp
SetTemp
Power
Mode
Speed
Alarm
Last update
```

Пустая retained `Temp` отображается как:

```text
—
```

`Alarm=2` показывается как отсутствие связи.

## 35. Индивидуальные команды

Доступны:

```text
Power
Mode
Speed
SetTemp
Blinds
Blok
```

Mapping:

| Control | Values |
|---|---|
| `Power` | `0`, `1` |
| `Mode` | `0..4` |
| `Speed` | `1..4` |
| `SetTemp` | `16..32` |
| `Blinds` | `0`, `1` |
| `Blok` | `0`, `1` |

Topic:

```text
/devices/Fan-<bus>_<address>/controls/<Control>/on1
```

Команда non-retained.

## 36. Когда управление отключено

Browser не отправляет команду, если:

- MQTT disconnected;
- factual Status ещё не получен;
- устройство offline;
- предыдущая команда этого control ещё ждёт confirmation.

Disabled button не означает, что dashboard повреждён. Проверьте connection и factual state.

## 37. Подтверждение команды

После publish browser не меняет factual state оптимистично.

Он ждёт base topic:

```text
/devices/Fan-B_A/controls/<Control>
```

с ожидаемым значением.

UI timeout:

```text
10 seconds
```

При timeout:

- команда помечается неподтверждённой;
- последнее factual значение остаётся на экране;
- driver продолжает обычный polling;
- desired value не выдаётся за факт.

## 38. SetTemp в Auto

Фанкойл может принять C3 response, но не изменить SetTemp в C0.

В таком случае:

- UI продолжит показывать старую factual уставку;
- driver выполнит ограниченные повторы;
- после лимита бесконечного цикла не будет;
- устройство останется online, если отвечает на C0;
- UI может показать timeout подтверждения.

Это ожидаемое безопасное поведение.

## 39. Групповой режим

Групповой режим позволяет выбрать markers и применить:

```text
Power
Mode
Speed
SetTemp
```

Параметры включаются независимо.

Можно отправить:

- только Power;
- только SetTemp;
- Mode + Speed;
- Power + Mode + Speed + SetTemp;
- любую другую комбинацию выбранных controls.

## 40. Выбор группы

Доступны:

- выбор marker;
- выбрать все visible;
- очистить выбор.

Hidden markers не входят в «выбрать все visible».

Один физический fan на panel представлен одним marker.

## 41. Пропущенные групповые команды

При построении плана browser пропускает:

- waiting devices;
- offline devices;
- devices при отключённом MQTT;
- control, уже ожидающий confirmation.

Остальные команды публикуются индивидуально.

MDV broadcast не используется.

## 42. Групповое подтверждение

Каждая операция подтверждается отдельно factual topic.

Пример для двух устройств и трёх controls:

```text
2 × 3 = 6 MQTT commands
6 отдельных factual confirmations
```

Частичный успех возможен.

UI должен показывать выполненные, ожидающие, пропущенные и не подтвердившиеся операции отдельно.

## 43. Retained factual state

Страница подписана:

```text
/devices/+/controls/+
```

Она не подписывается на `/on1` как на factual state.

При открытии retained values позволяют сразу восстановить последнее подтверждённое состояние.

Доступность обновляется driver и offline publisher.

## 44. Что происходит при отключении MQTT

При разрыве:

- connection badge становится offline;
- новые команды блокируются;
- последние полученные values остаются видимыми;
- активные ожидания upload завершаются ошибкой;
- reconnect выполняется автоматически через 2 seconds;
- subscriptions создаются повторно.

Старые non-retained команды не воспроизводятся после reconnect.

## 45. Диагностика: страница не открывается

Проверьте:

```bash
ls -la /var/www/mdvwb
ls -la /var/www/fancoils
```

HTTP:

```bash
curl -I http://127.0.0.1/mdvwb/
curl -I http://127.0.0.1/fancoils/
```

Проверьте завершающий `/`.

## 46. Диагностика: MQTT offline

Проверьте broker:

```bash
systemctl status mosquitto.service --no-pager
```

WebSocket endpoint:

```text
/mqtt
```

Откройте browser developer tools и проверьте WebSocket connection.

Обычный MQTT port `1883` и WebSocket endpoint — разные transport endpoints одного broker.

## 47. Диагностика: dashboard не загружается

Проверьте:

```bash
systemctl status mdvwb-manager.service --no-pager
journalctl -u mdvwb-manager.service -n 100 --no-pager
```

Files:

```bash
ls -la /etc/mdvwb/dashboard.json
ls -la /var/www/fancoils/assets
```

MQTT:

```bash
mosquitto_sub -v -t '/mdvwb/dashboard/#'
```

## 48. Диагностика: revision conflict

Признаки:

```text
Dashboard revision conflict
локальный draft сохранён
server configuration обновлена
```

Действия:

1. Просмотрите изменения текущего draft.
2. Сравните с новой server version.
3. Нажмите отмену, если нужен server вариант.
4. Повторно сохраните осознанно, если нужен локальный вариант.

Не обновляйте страницу до оценки draft, иначе локальные изменения могут быть потеряны браузером.

## 49. Диагностика: upload завис

Подпишитесь:

```bash
mosquitto_sub -v \
  -t '/mdvwb/dashboard/background/upload/status' \
  -t '/mdvwb/dashboard/background/upload/result'
```

Проверьте:

- MQTT connection;
- current revision;
- размер файла;
- последовательность chunk indexes;
- отсутствие второго concurrent upload;
- доступность записи в assets directory;
- journal manager.

Cancel active upload из editor и начните заново после получения current dashboard.

## 50. Диагностика: marker не управляется

Проверьте:

- marker `visible=true`;
- device имеется в `buses.json`;
- driver service active;
- factual `Status` получен;
- `Status != 7`;
- `Alarm != 2`;
- MQTT connected;
- нет pending command того же control.

MQTT:

```bash
mosquitto_sub -v -t '/devices/Fan-<bus>_<address>/controls/#'
```

## 51. Безопасность эксплуатации

- Не публикуйте retained-команды в `/on1`.
- Не редактируйте `dashboard.json` во время работы manager без последующей проверки revision.
- Не копируйте произвольные файлы в dashboard config через path.
- Не запускайте несколько background uploads одновременно.
- Не считайте C3 response подтверждением изменения.
- Не включайте offline устройства в критичные групповые операции без контроля результата.
- Делайте backup `/etc/mdvwb` и `/var/www/fancoils/assets`.

## 52. Где находятся расписания

Редактор расположен в drawer страницы:

```text
/fancoils/?panel=<panel-id>
```

Список показывает schedules только открытой panel, но файл общий для всего проекта:

```text
/etc/mdvwb/schedules.json
```

## 53. Требования для работы

Должны работать:

```text
mosquitto.service
mdvwb-manager.service
mdvwb-scheduler.service
нужные mdvwb@<bus>.service
```

Browser получает config/status через MQTT WebSocket.

## 54. Список расписаний

Запись показывает:

- name;
- timing;
- targets count;
- enabled;
- последний полученный run state.

States:

```text
queued
executing
completed
timeout
failed
rejected
missed
```

Run result non-retained и после reload может отсутствовать.

## 55. Новый schedule

Default draft:

```text
ID = schedule-N
Name = Новое расписание
Enabled = true
Kind = weekly
Days = Monday..Friday
Time = browser time + 5 minutes
Actions = Power On
Targets = none
```

До выбора target запись не пройдёт validation.

## 56. ID и name

ID:

```text
1..64 characters
A-Z a-z 0-9 _ -
```

ID глобально уникален между panels.

Name:

```text
1..128 bytes
```

## 57. Weekly

Выберите время и минимум один день.

```text
1 Пн
2 Вт
3 Ср
4 Чт
5 Пт
6 Сб
7 Вс
```

Дата для weekly не используется.

## 58. Once

Выберите:

```text
date = YYYY-MM-DD
time = HH:MM
```

Backend допускает years `2000..2099` и проверяет реальную календарную дату.

Запись после выполнения остаётся в конфигурации.

## 59. Enabled и manual run

`enabled=true` разрешает automatic execution.

Manual run разрешён и для disabled schedule. Это позволяет хранить шаблоны без автоматического старта.

## 60. Targets

Targets выбираются кликами по markers текущей panel.

Доступны только placements:

```text
visible = true
```

Target хранит физическую пару `bus/address`, а user number и label используются для отображения.

Максимум 512 targets.

## 61. Actions

Можно включить любую комбинацию:

```text
Power
Mode
Speed
SetTemp
```

Неотмеченный action не изменяется.

Допустимые значения:

```text
Power = Off | On
Mode = Cool | Heat | Dry | Fan | Auto
Speed = Low | Medium | High | Auto
SetTemp = 16..32
```

Хотя бы один action обязателен.

## 62. Порядок команд

Для каждой target scheduler отправляет:

```text
Mode
Speed
SetTemp
Power
```

Power идёт последним. Broadcast не используется.

## 63. Save

Browser отправляет весь общий file:

```text
/mdvwb/schedules/config/set
```

Сохраняются entries других panels. Manager проверяет schema, revision и references, затем atomically записывает file и увеличивает revision.

## 64. Revision conflict

При параллельном save:

1. stale request отклоняется;
2. file не меняется;
3. current config публикуется повторно;
4. dirty draft остаётся открыт.

Automatic merge отсутствует.

## 65. Duplicate и delete

Duplicate создаёт новый ID, копирует timing/targets/actions и остаётся unsaved.

Unsaved draft удаляется локально. Persisted entry удаляется только через save общей configuration.

## 66. Когда доступен «Запустить сейчас»

Текущий `app.js` разрешает кнопку, когда:

- schedule сохранён;
- draft clean;
- MQTT connected;
- save не выполняется;
- scheduler status имеет `ready`, `executing` или `warning`.

Enabled не обязателен.

Текущая page не проверяет свежесть scheduler heartbeat. Retained status после reconnect может временно оставить кнопку доступной, даже если process уже не работает.

## 67. Время WB

Automatic due использует controller clock, а не browser clock.

Scheduler status содержит:

```text
controllerDate
controllerTime
controllerMinute
controllerEpoch
controllerWeekday
```

Текущая `/fancoils/` page не показывает отдельный badge «Время WB», потому что helper `scheduler-status-ui.js` не подключён.

Проверка:

```bash
date
timedatectl status
mosquitto_sub -C 1 -v -t '/mdvwb/scheduler/status'
```

Browser preview нового schedule по-прежнему использует локальное время компьютера.

## 68. Текущее scheduler-status поведение

В repository есть:

```text
scheduler-status-ui.js
scheduler-status-health.js
```

Эти helpers умеют различать retained и live heartbeat и используют threshold 125 seconds.

Однако текущий `/fancoils/index.html` загружает только `app.js`, а `app.js` helpers не импортирует.

Поэтому production page сейчас:

- отображает последний scheduler state;
- не различает retained/live;
- не вычисляет stale age;
- не блокирует manual run из-за отсутствия fresh heartbeat.

Проверяйте живое состояние через `systemctl` и новый non-retained MQTT status.

## 69. Текущий manual run lifecycle

Browser публикует:

```text
/mdvwb/schedules/<id>/run
payload = run
retain = false
```

Manager и scheduler публикуют общий:

```text
/mdvwb/schedules/<id>/result
```

Scheduler result содержит:

```json
"origin": "scheduler"
```

Но текущий `app.js` не разделяет результаты по `origin`. Он сохраняет и показывает последнее state/message для schedule.

Manager acceptance ещё не означает отправку commands. Для строгой диагностики подпишитесь на result topic и дождитесь сообщения `origin="scheduler"` вручную.

## 70. Текущее ожидание результата в UI

Current production page не устанавливает отдельный 90-second safety timeout для manual run.

Backend scheduler confirmation timeout по умолчанию:

```text
10 seconds
```

Schedule может дольше ждать своей очереди до начала выполнения. Если UI долго не получает terminal result, проверьте:

```bash
systemctl status mdvwb-scheduler.service --no-pager
mosquitto_sub -v -t '/mdvwb/schedules/<id>/result'
```

Dormant `scheduler-status-ui.js` содержит 90-second client timeout, но он пока не подключён.

## 71. Automatic run

Weekly due:

```text
enabled + weekday + HH:MM
```

Once due:

```text
enabled + YYYY-MM-DD + HH:MM
```

Browser может быть закрыт.

## 72. Защита от повторного запуска

Scheduler сохраняет:

```text
/var/lib/mdvwb/scheduler-state.tsv
```

Restart в той же due minute не повторяет automatic commands.

Manual run этим state не блокируется.

## 73. Missed once

Если one-time schedule уже в прошлом:

```text
state = missed
```

Команды поздно не отправляются. Marker сохраняется и не повторяется каждый Tick.

## 74. Factual confirmation

Scheduler ждёт base topics:

```text
/devices/Fan-B_A/controls/Power
/devices/Fan-B_A/controls/Mode
/devices/Fan-B_A/controls/Speed
/devices/Fan-B_A/controls/SetTemp
```

Нужно matching value после command и `retained=false`.

Старое retained значение не подтверждает run.

## 75. Timeout и partial execution

Timeout:

```text
Confirmation timeout: X/Y values confirmed
```

Rollback нет. Часть devices/actions могла примениться.

После timeout проверяйте factual state каждого target.

## 76. Offline target

Fresh:

```text
Status=7
```

любой active target завершает весь run как `failed`.

Retained historical offline state новый run автоматически не завершает.

## 77. Очередь

Одновременно выполняется одна schedule.

Лимит run queue:

```text
128
```

Повторный queued request того же ID заменяет предыдущий.

## 78. Изменение configuration во время run

Stale queued revision отклоняется до commands.

Изменение accepted schedules во время active execution завершает run как `failed`.

## 79. Изменение buses/dashboard

Scheduler постоянно проверяет три JSON files.

Удаление/disable bus, address, panel или visible placement может:

- блокировать scheduler;
- очистить queue;
- завершить active run;
- запретить manual/automatic execution.

После valid repair scheduler восстанавливается автоматически.

## 80. Manager status и runtime status

Не путайте:

```text
/mdvwb/schedules/status
/mdvwb/scheduler/status
```

Первый описывает stored configuration. Второй — runtime process и controller clock.

## 81. Result states

Terminal:

```text
completed
timeout
failed
rejected
missed
```

Non-terminal:

```text
queued
executing
```

Scheduler result имеет `origin=scheduler`; текущий `app.js` этот признак отдельно не фильтрует.

## 82. Пример рабочего утра

```json
{
  "id": "workday-start",
  "name": "Начало рабочего дня",
  "enabled": true,
  "panelId": "main",
  "kind": "weekly",
  "days": [1, 2, 3, 4, 5],
  "date": "",
  "time": "08:00",
  "targets": [
    {"bus": 1, "address": 1},
    {"bus": 1, "address": 3}
  ],
  "actions": {
    "power": true,
    "mode": 0,
    "speed": 2,
    "setTemp": 23
  }
}
```

## 83. Пример вечернего выключения

```json
{
  "id": "workday-stop",
  "name": "Конец рабочего дня",
  "enabled": true,
  "panelId": "main",
  "kind": "weekly",
  "days": [1, 2, 3, 4, 5],
  "date": "",
  "time": "19:00",
  "targets": [{"bus": 1, "address": 1}],
  "actions": {"power": false}
}
```

## 84. Диагностика scheduler

```bash
systemctl status mdvwb-scheduler.service --no-pager
journalctl -u mdvwb-scheduler.service -n 150 --no-pager
cat /etc/default/mdvwb-scheduler
```

## 85. Диагностика времени

```bash
date
timedatectl status
```

Исправьте timezone и sync до эксплуатации automatic schedules.

## 86. Диагностика MQTT

```bash
mosquitto_sub -v \
  -t '/mdvwb/schedules/#' \
  -t '/mdvwb/scheduler/status'
```

Factual state:

```bash
mosquitto_sub -v -t '/devices/Fan-1_3/controls/+'
```

## 87. Ручной тест

Terminal 1:

```bash
mosquitto_sub -v -t '/mdvwb/schedules/workday-start/result'
```

Terminal 2:

```bash
mosquitto_pub -t '/mdvwb/schedules/workday-start/run' -m 'run'
```

Не добавляйте retained flag.

## 88. Scheduler показывает error

Частые причины:

- invalid JSON;
- missing panel/bus/address;
- hidden target;
- disabled target bus;
- недоступен configuration file.

После исправления дождитесь нового live status.

## 89. Automatic schedule не запускается

Проверьте:

- enabled;
- WB clock/timezone;
- days/date/time;
- service active;
- bus enabled;
- placement visible;
- state marker текущей minute;
- scheduler status.

## 90. Manual run заблокирован

Проверьте:

- persisted schedule;
- clean draft;
- MQTT;
- pending save;
- scheduler status имеет `ready`, `executing` или `warning`.

Текущая page не блокирует кнопку по возрасту heartbeat. Если retained status выглядит доступным, но process не отвечает, проверьте:

```bash
systemctl status mdvwb-scheduler.service --no-pager
mosquitto_sub -v -t '/mdvwb/scheduler/status'
```

## 91. Run уходит в timeout

Проверьте:

- driver process;
- C0 responses;
- factual non-retained publications;
- SetTemp в текущем mode;
- target offline state;
- MQTT transport;
- число targets.

Timeout environment:

```text
MDVWB_SCHEDULER_CONFIRM_TIMEOUT=10
```

Range `1..300`.

## 92. Partial execution

После `failed` или `timeout`:

1. проверьте factual values;
2. не повторяйте весь schedule вслепую;
3. повторите только нужные actions;
4. проверьте RS-485 и MQTT logs.

## 93. Backup

Сохраняйте:

```text
/etc/mdvwb/schedules.json
/var/lib/mdvwb/scheduler-state.tsv
```

При переносе проверьте IDs, targets, timezone и старые state markers.

## 94. Безопасность

- Не публикуйте retained `/run` или `/execute`.
- Не считайте manager `queued` завершением.
- Не считайте retained facts подтверждением.
- Не удаляйте state file в due minute.
- Не скрывайте target без проверки schedules.
- Не ожидайте rollback нескольких targets.

## 95. Ограничение manager error counts

При schema/reference error manager сейчас может вернуть нулевые revision/counts.

Не заменяйте local server state этими числами. Дождитесь retained `/mdvwb/schedules/config`.

## 96. Проверка ссылок после dashboard edit

После rename/delete panel или изменения visible markers проверьте:

```text
/mdvwb/schedules/status
referenceIssues
```

Исправьте schedules до использования runtime.

## 97. Проверка после изменения buses

После remove/disable bus или address проверьте оба статуса:

```text
/mdvwb/schedules/status
/mdvwb/scheduler/status
```

Stored configuration может быть warning, а runtime scheduler — blocked/error.

## 98. Installation и эксплуатация

Актуальная production-инструкция:

```text
docs/INSTALLATION.md
```

Она описывает:

- ARM64 offline package;
- checksums;
- обязательный backup;
- update и rollback;
- systemd units;
- manager/scheduler environment;
- `/mqtt`;
- permissions;
- controller diagnostics;
- удаление.

Release verification:

```text
docs/RELEASE_CHECKLIST.md
```

## 99. Важные deployment ограничения

- MDVWB installer не настраивает Mosquitto WebSocket `/mqtt`.
- Offline installer сохраняет non-empty JSON и uploaded assets, но не создаёт backup.
- Automatic rollback отсутствует.
- Production installation выполняется offline ARM64 package.
- Текущий online source installer не устанавливает полный `/fancoils/` application.
- `scheduler-status-ui.js` и `scheduler-status-health.js` поставляются, но текущий entry point их не загружает; наличие файлов не означает активный heartbeat UI.
- Offline package required-file list и workflows не везде явно проверяют `scheduler-status-health.js`.
- Перед update сохраняйте `/etc/mdvwb`, scheduler state и assets.
- После installation проверяйте manager, scheduler и каждый expected `mdvwb@N`.

## 100. Documentation status

Разделы driver, manager, dashboard, direct control, group control, schedules и browser behavior актуализированы по текущей версии проекта.

Installation и release procedure находятся в отдельных operational documents, чтобы команды восстановления и проверки не смешивались с пользовательским web guide.
