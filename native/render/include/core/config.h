// config.json: те саме, що читає й пише Python (hominka/config.py).
//
// Файл один на програму, і поки Python ще живий, писати в нього можуть двоє.
// Тому тут ключове правило: ми НЕ переписуємо файл своїм уявленням про нього,
// а міняємо в ньому свої ключі й лишаємо решту як була. Інакше перший же наш
// запис мовчки з'їв би налаштування, яких ми ще не знаємо, — і людина
// втратила б їх, просто посунувши вікно.
//
// Місце файлу теж те саме: %LOCALAPPDATA%\Hominka під Windows,
// $XDG_CONFIG_HOME/hominka (або ~/.config/hominka) на решті. Різні місця
// означали б, що після переходу на нативну версію налаштування «зникли».
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "core/look.h"

namespace hominka {

// Все, що програмі треба знати між запусками.
struct Config {
    // Вікно
    int x = 60, y = 60, w = 430, h = 560;
    Look look;                       // opacity, bg_alpha, zoom, frameless, locked
    bool keep_top = true;

    // Чат
    std::string youtube;             // «@нік», UC-id або посилання
    std::string twitch;
    std::string kick;
    std::string site_url;            // свій чат за посиланням (сюди не читаємо)
    std::string custom_css;
    std::vector<std::string> layout; // порядок частин рядка
    int chat_delay = 0;              // секунди

    // Чат у грі
    int game_opacity = 235;
    bool game_hide_obs = false;

    // Оновлення
    std::string channel = "stable";
    bool auto_update = true;

    std::string path() const { return path_; }

    // Читає файл. Відсутній або зіпсований — лишаються типові значення: чат
    // має піднятися й без налаштувань, а не впасти через чужий JSON.
    void load();

    // Пише файл, зберігаючи чужі ключі. Виклики частішають під час
    // перетягування вікна, тож справжній запис відкладено: save() лише
    // позначає «треба», а flush() пише, якщо минув час.
    void save();
    void flush(bool force = false);

    bool dirty() const { return dirty_; }

private:
    std::string path_;
    nlohmann::json raw_;             // те, що було у файлі, разом із чужим
    bool dirty_ = false;
    int64_t dirty_since_ = 0;
};

// Тека даних програми — та сама, що в hominka/paths.py.
std::string data_dir();

}  // namespace hominka
