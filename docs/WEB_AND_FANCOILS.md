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

## 52. Что будет дополнено на следующем шаге

Следующий документационный шаг полностью восстановит:

- schema `schedules.json`;
- weekly и once;
- scheduler execution;
- manual run;
- target/action confirmation;
- scheduler freshness;
- UI редактора расписаний;
- диагностику расписаний.

До этого момента разделы dashboard и прямого управления в данном документе соответствуют текущей реализации.
