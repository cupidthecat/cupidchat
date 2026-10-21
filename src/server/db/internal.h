#pragma once
#include "server/db.h"
#include <sqlite3.h>

struct cupid_db {
    sqlite3 *handle;
};
