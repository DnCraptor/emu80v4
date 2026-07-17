# Карта зависимостей накопителей Вектор-06Ц

## Источник истины

Сопоставлены:

- исходный импорт emu80v4 (`169b28b`, `dist/vector/vector.conf` и `src/pico/vector06c_config.h`);
- подтверждённое дерево после 1.46;
- дерево после виртуального применения 1.47.

## Дисководы A и B

Цепочка:

`SysReq SR_DISKA/SR_DISKB -> Platform::m_diskA/m_diskB -> FdImage -> Fdc1793 -> VectorFddControlRegister -> порты 0x18..0x1C`

В исходной конфигурации Вектора создаются только два образа дискет: `diskA` и `diskB`.

`diskC` и `diskD` — универсальные поля оболочки Emu80, но не части HDD-контроллера Вектора.

После проверки трёх направлений зависимостей они удалены в итерации 1.49 вместе с `SR_DISKC/SR_DISKD` и сочетаниями `Shift+Alt+C/D`. Цепочки A/B, HDD, EDD и EDD2 не изменялись.

## HDD Вектора

Цепочка:

`SysReq SR_HDD -> Platform::m_hdd -> DiskImage("HDD") -> AtaDrive -> VectorHddRegisters -> порты 0x50..0x5F`

HDD не связан с `diskC` или `diskD`. Удаление команд дисководов C/D не должно удалять HDD, но сама такая чистка должна выполняться отдельно и только после сохранения цепочки выше.

## EDD

Цепочка выполнения:

`порт 0x10 -> VectorRamDiskSelector(diskNum=0) -> VectorAddrSpace::ramDiskControl(0, ...) -> SRam ramDiskMem (256 КиБ)`

Цепочка файловых операций:

`SysReq SR_*RAMDISK -> Platform::m_ramDisk -> RamDisk -> страница 0 -> SRam ramDiskMem`

В итерации 1.34 была перенесена только первая цепочка. Объект `RamDisk` не создавался, поэтому загрузка/сохранение EDD через интерфейс фактически не работали.

## EDD2

Цепочка выполнения:

`порт 0x11 -> VectorRamDiskSelector(diskNum=1) -> VectorAddrSpace::ramDiskControl(1, ...) -> SRam ramDiskMem2 (256 КиБ)`

Цепочка файловых операций:

`SysReq SR_*RAMDISK2 -> Platform::m_ramDisk2 -> RamDisk(label="EDD2") -> страница 0 -> SRam ramDiskMem2`

В итерации 1.34 вся эта цепочка была ошибочно удалена при отказе от конфигурационного парсера.

## Аудит удалений 1.44–1.47

Удалённые реализации других машин, их встроенные ресурсы и модули `Pic8259`, `PpiAtaAdapter`, `RkFdd`, `RkRomDisk`, `RkSdController`, `SdCard`, `SdAdapters`, `RkKeyboard`, `RkPpi8255Circuit`, `MsxTapeHooks`, `RfsTapeHooks` и `GenericModules` не создаются исходной конфигурацией Вектора.

`RkTapeHooks` сохранён правильно: он используется кассетными перехватчиками Вектора.

## Правило дальнейшего удаления

Объект или интерфейс можно удалять только после проверки сразу по трём направлениям:

1. создание в исходной конфигурации Вектора;
2. создание в текущей статической модели `Platform`;
3. обращения из `SysReq`, горячих клавиш, адресных пространств и контроллеров портов.
