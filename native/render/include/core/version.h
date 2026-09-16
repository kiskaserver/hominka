// Хто ми і якої ми версії — для нативної частини.
//
// Правда лишається одна, у hominka/version.py: release.py проставляє номер там
// і копіює його СЮДИ (шукає рядок нижче тим самим способом). Тримати число в
// двох місцях довелося тому, що збірка нативної частини бачить лише теку
// native/ — Python-коду в контейнері немає.
#pragma once

#define HOMINKA_VERSION "3.2.1"
#define HOMINKA_NAME "Hominka"

// Ті самі числа окремо — для ресурсу версії Windows: у VERSIONINFO номер
// мусить бути чотирма числами, а не рядком, і розібрати рядок засобами
// windres не можна.
#define HOMINKA_VER_MAJOR 3
#define HOMINKA_VER_MINOR 2
#define HOMINKA_VER_PATCH 1
