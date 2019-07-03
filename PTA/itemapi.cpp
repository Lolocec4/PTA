#include "itemapi.h"
#include "pitem.h"
#include "pta_types.h"
#include "statdialog.h"

#include <unordered_set>

#include <nlohmann/json.hpp>

#include <QDebug>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>

using json = nlohmann::json;

// PoE trade api only allows 10 items at once
constexpr size_t papi_query_limit = 10;

// API URLs
const QUrl    leagueApiUrl("https://www.pathofexile.com/api/trade/data/leagues");
const QUrl    modsApiUrl("https://www.pathofexile.com/api/trade/data/stats");
const QUrl    itemsApiUrl("https://www.pathofexile.com/api/trade/data/items");
const QUrl    repoeBaseUrl("https://raw.githubusercontent.com/brather1ng/RePoE/master/data/base_items.min.json");
const QString tradeFetchUrl("https://www.pathofexile.com/api/trade/fetch/%1?query=%2");
const QString tradeSearchUrl("https://www.pathofexile.com/api/trade/search/");
const QString tradeSiteUrl("https://www.pathofexile.com/trade/search/");

ItemAPI::ItemAPI(QObject* parent) : QObject(parent)
{
    m_manager = new QNetworkAccessManager(this);

    // Download leagues

    auto rp_league = m_manager->get(QNetworkRequest(leagueApiUrl));
    connect(rp_league, &QNetworkReply::finished, [=]() {
        rp_league->deleteLater();

        if (rp_league->error() != QNetworkReply::NoError)
        {
            qWarning() << "PAPI: Error downloading league data" << rp_league->error() << rp_league->errorString();
            return;
        }

        auto  obj = json::parse(rp_league->readAll().toStdString());
        auto& rs  = obj["result"];

        m_leagues = json::array();
        for (size_t i = 0; i < rs.size(); i++)
        {
            m_leagues.push_back(rs[i]["id"].get<std::string>());
        }

        QString setlg = getLeague();

        qInfo() << "League data loaded. Setting league to" << setlg;
    });

    // Download stats

    auto rp_stats = m_manager->get(QNetworkRequest(modsApiUrl));
    connect(rp_stats, &QNetworkReply::finished, [=]() {
        rp_stats->deleteLater();

        if (rp_stats->error() != QNetworkReply::NoError)
        {
            qWarning() << "PAPI: Error downloading mod data" << rp_stats->error() << rp_stats->errorString();
            return;
        }

        auto obj = json::parse(rp_stats->readAll().toStdString());

        auto& ll = obj["result"];

        for (auto& type : ll)
        {
            auto& el = type["entries"];

            for (auto& et : el)
            {
                m_stats_by_text.insert({{et["text"].get<std::string>(), et}});
                m_stats_by_id.insert({{et["id"].get<std::string>(), et}});
            }
        }

        qInfo() << "Mod data loaded";
    });

    // Download unique items

    auto rp_uniq = m_manager->get(QNetworkRequest(itemsApiUrl));
    connect(rp_uniq, &QNetworkReply::finished, [=]() {
        rp_uniq->deleteLater();

        if (rp_uniq->error() != QNetworkReply::NoError)
        {
            qWarning() << "PAPI: Error downloading unique item data" << rp_uniq->error() << rp_uniq->errorString();
            return;
        }

        // Process data
        auto obj = json::parse(rp_uniq->readAll().toStdString());

        auto& ll = obj["result"];

        for (auto& type : ll)
        {
            auto& el = type["entries"];

            for (auto& et : el)
            {
                if (et.contains("name"))
                {
                    m_uniques.insert({{et["name"].get<std::string>(), et}});
                }
                else if (et.contains("type"))
                {
                    m_uniques.insert({{et["type"].get<std::string>(), et}});
                }
                else
                {
                    qDebug() << "Item entry has neither name nor type:" << QString::fromStdString(et.dump());
                }
            }
        }

        qInfo() << "Unique item data loaded";
    });

    // Load base categories
    QFile bc("data/base_categories.json");

    if (!bc.open(QIODevice::ReadOnly))
    {
        throw std::runtime_error("Cannot open base_categories.json");
    }

    QByteArray bdat = bc.readAll();

    c_baseCat = json::parse(bdat.toStdString());

    qInfo() << "Base categories loaded";

    // Load RePoE base data

    auto rp_base = m_manager->get(QNetworkRequest(repoeBaseUrl));
    connect(rp_base, &QNetworkReply::finished, [=]() {
        rp_base->deleteLater();

        if (rp_base->error() != QNetworkReply::NoError)
        {
            qWarning() << "PAPI: Error downloading item base data" << rp_base->error() << rp_base->errorString();
            return;
        }

        // Process data
        auto obj = json::parse(rp_base->readAll().toStdString());

        for (auto& [k, o] : obj.items())
        {
            std::string typeName  = o["name"].get<std::string>();
            std::string itemClass = o["item_class"].get<std::string>();

            auto search = c_baseCat.find(itemClass);
            if (search != c_baseCat.end())
            {
                std::string itemCat = search.value().get<std::string>();

                c_baseMap.insert({{typeName, itemCat}});
            }
        }

        qInfo() << "Item base data loaded";
    });

    // Load pseudo rules
    QFile pr("data/pseudo_rules.json");

    if (!pr.open(QIODevice::ReadOnly))
    {
        throw std::runtime_error("Cannot open pseudo_rules.json");
    }

    QByteArray pdat = pr.readAll();

    c_pseudoRules = json::parse(pdat.toStdString());

    qInfo() << "Pseudo rules loaded";
}

int ItemAPI::readPropInt(QString prop)
{
    // Remove augmented tag
    prop.replace(" (augmented)", "");

    QRegularExpression      re("^([\\+\\-]?[\\d\\.]+)%?");
    QRegularExpressionMatch match = re.match(prop);

    if (match.hasMatch())
    {
        QString val = match.captured(1);
        return val.toInt();
    }

    return 0;
}

mmv_t ItemAPI::readPropIntRange(QString prop)
{
    mmv_t val = {0, 0};

    // If it is a list, process list
    if (prop.contains(", "))
    {
        // Split and recursively read
        auto list = prop.split(", ", QString::SkipEmptyParts);

        for (auto& item : list)
        {
            val += readPropIntRange(item);
        }

        return val;
    }

    // Otherwise process single entry

    // Remove augmented tag
    prop.replace(" (augmented)", "");

    QRegularExpression      re("^(\\d+)-(\\d+)");
    QRegularExpressionMatch match = re.match(prop);

    if (match.hasMatch())
    {
        QString v1 = match.captured(1);
        QString v2 = match.captured(2);

        val.min = v1.toInt();
        val.max = v2.toInt();
    }

    return val;
}

double ItemAPI::readPropFloat(QString prop)
{
    // Remove augmented tag
    prop.replace(" (augmented)", "");

    QRegularExpression      re("^([\\+\\-]?[\\d\\.]+)%?");
    QRegularExpressionMatch match = re.match(prop);

    if (match.hasMatch())
    {
        QString val = match.captured(1);
        return val.toDouble();
    }

    return 0.0;
}

socket_filters_t ItemAPI::readSockets(QString prop)
{
    socket_filters_t ss = {0, {0, 0, 0, 0, 0}};

    auto llist = prop.split(" ", QString::SkipEmptyParts);

    for (auto& lpart : llist)
    {
        auto socks = lpart.split("-", QString::SkipEmptyParts);

        if (socks.length() > 1 && socks.length() > ss.links)
        {
            // New max links
            ss.links = socks.length();
        }

        for (auto& s : socks)
        {
            if ("R" == s)
            {
                ss.sockets.r++;
            }
            else if ("G" == s)
            {
                ss.sockets.g++;
            }
            else if ("B" == s)
            {
                ss.sockets.b++;
            }
            else if ("W" == s)
            {
                ss.sockets.w++;
            }
            else if ("A" == s)
            {
                ss.sockets.a++;
            }
        }
    }

    return ss;
}

int ItemAPI::readPropExp(QString prop)
{
    // TODO: read exp
    return 0;
}

std::string ItemAPI::readName(QString name)
{
    name.replace("<<set:MS>><<set:M>><<set:S>>", "");

    return name.toStdString();
}

std::string ItemAPI::readType(PItem* item, QString type)
{
    type.replace("<<set:MS>><<set:M>><<set:S>>", "");
    type.replace("Superior ", "");

    if (item->f_type.rarity == "Magic")
    {
        // Parse out magic affixes

        QRegularExpression      re("^\\S+ ([\\w\\s]+) of \\w+$");
        QRegularExpressionMatch match = re.match(type);

        if (match.hasMatch())
        {
            type = match.captured(1);
        }
    }

    return type.toStdString();
}

void ItemAPI::parseProp(PItem* item, QString prop)
{
    QString p = prop.section(":", 0, 0);
    QString v = prop.section(": ", 1, 1);

    if (!c_propMap.contains(p))
    {
        qDebug() << "Unknown/unimplemented prop:" << p;
        return;
    }

    auto& pev = c_propMap[p];

    switch (pev[0])
    {
        case weapon_filter:
        {
            switch (pev[1])
            {
                case weapon_filter_pdps:
                {
                    item->f_weapon.pdps = readPropIntRange(v);
                    break;
                }

                case weapon_filter_crit:
                {
                    item->f_weapon.crit = readPropFloat(v);
                    break;
                }

                case weapon_filter_aps:
                {
                    item->f_weapon.aps = readPropFloat(v);
                    break;
                }

                case weapon_filter_edps:
                {
                    item->f_weapon.edps = readPropIntRange(v);
                    break;
                }
            }

            item->is_weapon = true;

            break;
        }

        case armour_filter:
        {
            switch (pev[1])
            {
                case armour_filter_ar:
                {
                    item->f_armour.ar = readPropInt(v);
                    break;
                }

                case armour_filter_ev:
                {
                    item->f_armour.ev = readPropInt(v);
                    break;
                }

                case armour_filter_es:
                {
                    item->f_armour.es = readPropInt(v);
                    break;
                }

                case armour_filter_block:
                {
                    item->f_armour.block = readPropInt(v);
                    break;
                }
            }

            item->is_armour = true;

            break;
        }

        case socket_filter:
        {
            item->f_socket = readSockets(v);
            break;
        }

        case req_filter:
        {
            switch (pev[1])
            {
                case req_filter_lvl:
                {
                    item->f_req.lvl = readPropInt(v);
                    break;
                }

                case req_filter_str:
                {
                    item->f_req.str = readPropInt(v);
                    break;
                }

                case req_filter_dex:
                {
                    item->f_req.dex = readPropInt(v);
                    break;
                }

                case req_filter_int:
                {
                    item->f_req.intl = readPropInt(v);
                    break;
                }
            }

            break;
        }

        case misc_filter:
        {
            switch (pev[1])
            {
                case misc_filter_quality:
                {
                    item->f_misc.quality = readPropInt(v);
                    break;
                }

                case misc_filter_gem_level:
                {
                    item->f_misc.gem_level = readPropInt(v);
                    break;
                }

                case misc_filter_ilvl:
                {
                    item->f_misc.ilvl = readPropInt(v);
                    break;
                }

                case misc_filter_gem_level_progress:
                {
                    item->f_misc.gem_level_progress = readPropExp(v);
                    break;
                }
            }

            break;
        }

        case special_filter:
        {
            if (p == "Requirements")
            {
                m_section = "Requirements";
                break;
            }

            if (p == "Level")
            {
                QString fprop = "gem_level: ";

                if (m_section == "Requirements")
                {
                    fprop = "req_level: ";
                }

                QString cprop = fprop + v;
                parseProp(item, cprop);
                break;
            }

            qDebug() << "Unknown/unimplemented special prop:" << p;

            break;
        }
    }
}

bool ItemAPI::parseStat(PItem* item, QString stat, bool multiline)
{
    // Make a copy
    QString orig_stat = stat;

    if (stat == "Unidentified")
    {
        item->f_misc.identified = false;
        return true;
    }

    if (stat == "Shaper Item")
    {
        item->f_misc.shaper_item = true;
        return true;
    }

    if (stat == "Elder Item")
    {
        item->f_misc.elder_item = true;
        return true;
    }

    if (stat == "Corrupted")
    {
        item->f_misc.corrupted = true;
        return true;
    }

    bool stat_is_crafted = false;

    if (stat.endsWith("(crafted)"))
    {
        stat_is_crafted = true;
    }

    stat.replace(" (crafted)", "");

    // Get numeric values from stat
    json val = json::array();

    QRegularExpression              re("([\\+\\-]?[\\d\\.]+)");
    QRegularExpressionMatchIterator it = re.globalMatch(stat);

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();
        QString                 word  = match.captured(1);

        // Process floats
        if (word.contains('.'))
        {
            double numval = word.toDouble();
            val.push_back(numval);
        }
        else
        {
            int numval = word.toInt();
            val.push_back(numval);
        }
    }

    // Craft search token
    stat.replace(re, "#");

    auto stoken = stat.toStdString();
    bool found  = m_stats_by_text.contains(stoken);

    if (!found && val.size() && stat.contains("reduced"))
    {
        // If the stat line has a "reduced" value, try to
        // flip it and try again

        stat.replace("reduced", "increased");

        for (auto& v : val)
        {
            if (v.is_number_float())
            {
                v = v.get<double>() * -1.0;
            }
            else
            {
                v = v.get<int>() * -1;
            }
        }

        stoken = stat.toStdString();
        found  = m_stats_by_text.contains(stoken);
    }

    // Reverse replace search
    QString reverse_repl_stat = stat;
    while (!found && reverse_repl_stat.contains('#') && val.size())
    {
        // Try putting back some values in case the mod itself has hardcoded values
        reverse_repl_stat.replace(reverse_repl_stat.lastIndexOf("#"), 1, QString::number(val[val.size() - 1].get<int>()));

        stoken = reverse_repl_stat.toStdString();
        found  = m_stats_by_text.contains(stoken);

        if (found)
        {
            // Delete value used
            val.erase(val.size() - 1);
        }
    }

    // Forward replace search
    QString forward_repl_stat = stat;
    while (!found && forward_repl_stat.contains('#') && val.size())
    {
        // Try putting back some values in case the mod itself has hardcoded values
        forward_repl_stat.replace(forward_repl_stat.indexOf("#"), 1, QString::number(val[0].get<int>()));

        stoken = forward_repl_stat.toStdString();
        found  = m_stats_by_text.contains(stoken);

        if (found)
        {
            // Delete value used
            val.erase(0);
        }
    }

    if (!found)
    {
        // Try the original line
        stoken = orig_stat.toStdString();
        found  = m_stats_by_text.contains(stoken);

        if (found)
        {
            // If original line works, then mod has no variance
            val.clear();
        }
    }

    // Otherwise, give up
    if (!found)
    {
        if (!multiline)
        {
            // Could be a multiline stat
            if (m_section.isEmpty())
            {
                m_section = orig_stat;
            }
            else
            {
                // Try to combine and search again
                QString combstat = m_section + "\n" + orig_stat;

                bool result = parseStat(item, combstat, true);

                if (result)
                {
                    // Successfully parsed multiline stat
                    m_section.clear();
                }
                else
                {
                    // otherwise, ditch the older line
                    m_section = orig_stat;
                }

                return result;
            }
        }

        qDebug() << "Ignored/unprocessed line" << orig_stat;
        return false;
    }
    else if (!m_section.isEmpty())
    {
        // Clear section upon finding a good stat
        m_section.clear();
    }

    json filter;

    auto range = m_stats_by_text.equal_range(stoken);
    for (auto it = range.first; it != range.second; ++it)
    {
        auto& entry = it->second;

        if (stat_is_crafted)
        {
            if (entry["type"] != "crafted")
            {
                // skip this entry
                continue;
            }

            // use crafted stat
            filter["id"]    = entry["id"];
            filter["type"]  = entry["type"];
            filter["text"]  = entry["text"];
            filter["value"] = val;
            break;
        }
        else
        {
            if (entry["type"] == "pseudo")
            {
                // skip pseudos
                continue;
            }

            if (!filter.empty() && filter["type"] == "implicit" && entry["type"] == "explicit")
            {
                // prefer explicit?
                filter["id"]    = entry["id"];
                filter["type"]  = entry["type"];
                filter["text"]  = entry["text"];
                filter["value"] = val;
            }
            else if (filter.empty())
            {
                filter["id"]    = entry["id"];
                filter["type"]  = entry["type"];
                filter["text"]  = entry["text"];
                filter["value"] = val;
            }
            // else skip
        }
    }

    if (filter.empty())
    {
        qDebug() << "Error parsing stat line" << orig_stat;
        return false;
    }

    std::string fid = filter["id"].get<std::string>();

    // If the item already has this filter, merge them
    if (item->filters.contains(fid))
    {
        auto& efil = item->filters[fid];

        auto count = efil["value"].size();

        for (size_t i = 0; i < count; i++)
        {
            if (efil["value"][i].is_number_float())
            {
                efil["value"][i] = efil["value"][i].get<double>() + filter["value"][i].get<double>();
            }
            else
            {
                efil["value"][i] = efil["value"][i].get<int>() + filter["value"][i].get<int>();
            }
        }
    }
    else
    {
        item->filters.emplace(fid, filter);
    }

    return true;
}

void ItemAPI::processPriceResults(std::shared_ptr<PItem> item, json results)
{
    bool   done  = false;
    size_t start = 0, end = 0;
    auto   flist = results["result"];
    auto   total = flist.size();
    json   endRes;

    std::unordered_set<std::string> accounts;

    QSettings settings;
    size_t    display_limit = settings.value(PTA_CONFIG_DISPLAYLIMIT, PTA_CONFIG_DEFAULT_DISPLAYLIMIT).toInt();

    while (!done)
    {
        QStringList fetchcodes;

        if (total > papi_query_limit && total > end)
        {
            end += std::min(papi_query_limit, total - end);
        }
        else
        {
            end = total;
        }

        for (; start < end; start++)
        {
            fetchcodes.append(QString::fromStdString(flist.at(start)));
        }

        QString fcode = fetchcodes.join(',');

        QUrl resUrl = tradeFetchUrl.arg(fcode).arg(QString::fromStdString(results["id"]));

        QEventLoop loop;
        connect(m_manager, &QNetworkAccessManager::finished, &loop, &QEventLoop::quit);

        auto pricereq = m_manager->get(QNetworkRequest(resUrl));
        loop.exec();

        pricereq->deleteLater();

        if (pricereq->error() != QNetworkReply::NoError)
        {
            qWarning() << "PAPI: Error getting prices" << pricereq->error() << pricereq->errorString();
            return;
        }

        auto pricesb = pricereq->readAll();

        auto dat = json::parse(pricesb.toStdString());

        auto& rj = dat["result"];

        // Delete duplicate accounts
        // TODO make this a setting
        for (json::iterator it = rj.begin(); it != rj.end();)
        {
            auto& entry = *it;

            std::string accname = entry["listing"]["account"]["name"].get<std::string>();

            if (accounts.contains(accname))
            {
                // skip duplicate accounts
                it = rj.erase(it);
            }
            else
            {
                accounts.insert(accname);
                ++it;
            }
        }

        if (endRes.empty())
        {
            endRes = dat;
        }
        else
        {
            size_t limit = std::min(dat["result"].size(), display_limit - endRes["result"].size());

            for (size_t i = 0; i < limit; i++)
            {
                endRes["result"].push_back(dat["result"].at(i));
            }
        }

        done = (endRes["result"].size() == display_limit || total == end);
    }

    emit priceCheckFinished(item, QString::fromStdString(endRes.dump()));
}

QString ItemAPI::getLeague()
{
    QSettings settings;
    int       league = settings.value(PTA_CONFIG_LEAGUE, PTA_CONFIG_DEFAULT_LEAGUE).toInt();

    if (league > m_leagues.size())
    {
        QString defleag = QString::fromStdString(m_leagues[PTA_CONFIG_DEFAULT_LEAGUE].get<std::string>());

        qWarning() << "Previously set league no longer available. Resetting to default league" << defleag;

        league = PTA_CONFIG_DEFAULT_LEAGUE;
        settings.setValue(PTA_CONFIG_LEAGUE, PTA_CONFIG_DEFAULT_LEAGUE);
    }

    return QString::fromStdString(m_leagues[league].get<std::string>());
}

PItem* ItemAPI::parse(QString itemText)
{
    QTextStream stream(&itemText);
    QString     line;

    // Check first line for PoE item
    stream.readLineInto(&line);

    if (!line.startsWith("Rarity:"))
    {
        qWarning() << "Parse called on non PoE item text";
        return nullptr;
    }

    PItem* item = new PItem();

    // Full original text
    item->m_itemtext = itemText.toStdString();

    // Rarity
    item->f_type.rarity = line.section(": ", 1, 1).toStdString();

    // Read name/type
    QString nametype, type;
    stream.readLineInto(&nametype);
    stream.readLineInto(&type);

    if (type.startsWith("---"))
    {
        // nametype has to be item type and not name
        item->type = readType(item, nametype);
    }
    else
    {
        item->name = readName(nametype);
        item->type = readType(item, type);
    }

    // Process category
    if ("Gem" == item->f_type.rarity)
    {
        item->f_type.category = "gem";
    }
    else if ("Divination Card" == item->f_type.rarity)
    {
        item->f_type.category = item->f_type.rarity = "card";
    }

    if (item->type.ends_with("Map"))
    {
        item->f_type.category = "map";
    }

    if (item->f_type.category.empty() && m_uniques.contains(item->type))
    {
        auto search = m_uniques.find(item->type);
        if (search != m_uniques.end())
        {
            auto& je = search->second;
            if (je["type"] == "Prophecy")
            {
                // this is a prophecy
                item->name = item->type;
                item->type = item->f_type.category = "prophecy";
            }
        }
    }

    // Read the rest of the crap

    while (stream.readLineInto(&line))
    {
        // Skip
        if (line.startsWith("---"))
        {
            m_section.clear();
            item->m_sections++;
            continue;
        }

        if (line.contains(":"))
        {
            // parse item prop
            parseProp(item, line);
        }
        else if (item->m_sections > 1)
        {
            // parse item stat
            parseStat(item, line);
        }
    }

    if (item->f_type.category.empty())
    {
        auto base = c_baseMap.find(item->type);
        if (base != c_baseMap.end())
        {
            item->f_type.category = base->second;
        }
    }

    // Process special/pseudo rules
    if (item->filters.size())
    {
        for (auto [key, fil] : item->filters.items())
        {
            if (c_pseudoRules.contains(key))
            {
                auto& rules = c_pseudoRules[key];

                for (auto& r : rules)
                {
                    std::string pid = r["id"].get<std::string>();

                    auto pentry = m_stats_by_id[pid];

                    if (!item->pseudos.contains(pid))
                    {
                        json ps_entry = pentry;

                        ps_entry["value"] = json::array();

                        for (auto v : fil["value"])
                        {
                            if (v.is_number_float())
                            {
                                ps_entry["value"].push_back(v.get<double>() * r["factor"].get<double>());
                            }
                            else
                            {
                                ps_entry["value"].push_back((int) (v.get<int>() * r["factor"].get<double>()));
                            }
                        }

                        item->pseudos.emplace(pid, ps_entry);
                    }
                    else
                    {
                        auto& ps_entry = item->pseudos[pid];

                        for (size_t i = 0; i < fil["value"].size(); i++)
                        {
                            auto v = fil["value"][i];

                            // XXX: only support one operation right now
                            // also remove is useless
                            if (r["op"] == "add")
                            {
                                if (v.is_number_float())
                                {
                                    ps_entry["value"][i] = ps_entry["value"][i].get<double>() + (v.get<double>() * r["factor"].get<double>());
                                }
                                else
                                {
                                    ps_entry["value"][i] = ps_entry["value"][i].get<int>() + ((int) (v.get<int>() * r["factor"].get<double>()));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return item;
}

QString ItemAPI::toJson(PItem* item)
{
    json j;

    j["name"] = item->name;

    j["rarity"] = item->f_type.rarity;

    // process category
    if (!item->f_type.category.empty())
    {
        j["category"] = item->f_type.category;
    }

    if (item->name != item->type)
    {
        j["type"] = item->type;
    }

    j["sockets"] = item->f_socket.sockets.total();
    j["links"]   = item->f_socket.links;

    j["ilvl"]    = item->f_misc.ilvl;
    j["quality"] = item->f_misc.quality;

    if (item->f_type.category == "gem")
    {
        j["gem_level"] = item->f_misc.gem_level;
    }

    j["elder_item"]  = item->f_misc.elder_item;
    j["shaper_item"] = item->f_misc.shaper_item;
    j["identified"]  = item->f_misc.identified;
    j["corrupted"]   = item->f_misc.corrupted;

    if (!item->m_options.empty())
    {
        j["options"] = item->m_options;
    }

    return QString::fromStdString(j.dump());
}

void ItemAPI::simplePriceCheck(std::shared_ptr<PItem> item)
{
    if (item->f_type.category == "currency")
    {
        emit humour(tr("Currency search is unimplemented"));
        return;
    }

    QSettings settings;

    // TODO: default settings
    auto query = R"(
    {
        "query": {
            "status": {
                "option": "online"
            },
            "stats": [{
                "type": "and",
                "filters": []
            }]
        },
        "sort": {
            "price": "asc"
        }
    }
    )"_json;

    bool        is_unique_base = false;
    std::string searchToken;

    // Search by type if rare map, or if it has no name
    if ((item->f_type.category == "map" && item->f_type.rarity == "Rare") || item->name.empty())
    {
        is_unique_base = m_uniques.contains(item->type);
        searchToken    = item->type;
    }
    else
    {
        is_unique_base = m_uniques.contains(item->name);
        searchToken    = item->name;
    }

    // Force rarity if unique
    if (item->f_type.rarity == "Unique")
    {
        std::string rarity = item->f_type.rarity;
        std::transform(rarity.begin(), rarity.end(), rarity.begin(), ::tolower);

        query["query"]["filters"]["type_filters"]["filters"]["rarity"]["option"] = rarity;
    }

    // Force category
    if (!item->f_type.category.empty())
    {
        std::string category = item->f_type.category;
        std::transform(category.begin(), category.end(), category.begin(), ::tolower);

        query["query"]["filters"]["type_filters"]["filters"]["category"]["option"] = category;
    }

    // Check for unique items
    if (is_unique_base)
    {
        auto& qe = query["query"];

        auto range = m_uniques.equal_range(searchToken);
        for (auto it = range.first; it != range.second; ++it)
        {
            auto& entry = it->second;

            // If map, match default map discriminator and type
            if (item->f_type.category == "map")
            {
                if (entry["disc"] == m_mapdisc && entry["type"] == item->type)
                {
                    if (entry.contains("name"))
                    {
                        qe["name"] = {{"discriminator", entry["disc"]}, {"option", entry["name"]}};
                    }

                    qe["type"] = {{"discriminator", entry["disc"]}, {"option", entry["type"]}};

                    break;
                }
            }
            else if (entry["type"] == item->type)
            {
                // For everything else, just match type
                qe["type"] = entry["type"];

                if (entry.contains("name"))
                {
                    qe["name"] = entry["name"];
                }

                break;
            }
        }

        item->m_options = getLeague().toStdString();

        // Default Gem options
        if (item->f_type.category == "gem")
        {
            qe["filters"]["misc_filters"]["filters"]["gem_level"]["min"] = item->f_misc.gem_level;
            qe["filters"]["misc_filters"]["filters"]["quality"]["min"]   = item->f_misc.quality;

            item->m_options += ", Lv" + std::to_string(item->f_misc.gem_level) + "/" + std::to_string(item->f_misc.quality) + "%";
        }

        // Default socket options
        if (item->f_socket.sockets.total() == 6)
        {
            qe["filters"]["socket_filters"]["filters"]["sockets"]["min"] = item->f_socket.sockets.total();

            item->m_options += ", " + std::to_string(item->f_socket.sockets.total()) + "S";
        }

        // Default link options
        if (item->f_socket.links > 4)
        {
            qe["filters"]["socket_filters"]["filters"]["links"]["min"] = item->f_socket.links;

            item->m_options += ", " + std::to_string(item->f_socket.links) + "L";
        }

        // Default corrupt options
        bool corrupt_override = settings.value(PTA_CONFIG_CORRUPTOVERRIDE, PTA_CONFIG_DEFAULT_CORRUPTOVERRIDE).toBool();

        if (corrupt_override)
        {
            QString corrupt_search = settings.value(PTA_CONFIG_CORRUPTSEARCH, PTA_CONFIG_DEFAULT_CORRUPTSEARCH).toString();

            if (corrupt_search != "Any")
            {
                qe["filters"]["misc_filters"]["filters"]["corrupted"]["option"] = (corrupt_search == "Yes");

                item->m_options += ", Corrupted=" + corrupt_search.toStdString();
            }
            else
            {
                item->m_options += ", Corrupted=Any";
            }

            item->m_options += " (override)";
        }
        else
        {
            qe["filters"]["misc_filters"]["filters"]["corrupted"]["option"] = item->f_misc.corrupted;

            item->m_options += ", Corrupted=";
            item->m_options += item->f_misc.corrupted ? "Yes" : "No";
        }

        item->m_options += ", Mods ignored";

        auto qba = query.dump();

        QNetworkRequest request;
        request.setUrl(QUrl(tradeSearchUrl + getLeague()));
        request.setRawHeader("Content-Type", "application/json");

        auto req = m_manager->post(request, QByteArray::fromStdString(qba));
        connect(req, &QNetworkReply::finished, [=]() {
            req->deleteLater();

            if (req->error() != QNetworkReply::NoError)
            {
                qWarning() << "PAPI: Error querying trade site" << req->error() << req->errorString();
                return;
            }

            auto respdata = req->readAll();
            auto resp     = json::parse(respdata.toStdString());
            if (!resp.contains("result") || !resp.contains("id"))
            {
                emit humour(tr("Error querying trade site. See log for details"));
                qWarning() << "PAPI: Error querying trade site";
                qWarning() << "PAPI: Site responded with" << respdata;
                return;
            }

            if (resp["result"].size() == 0)
            {
                emit humour(tr("No results found."));
                qDebug() << "No results";
                return;
            }

            // else process the results
            processPriceResults(item, resp);
        });
    }
    else
    {
        // TODO: poeprices.info?
        emit humour(tr("Simple price check for rare items is unimplemented"));
        qWarning() << "Unimplemented";
    }
}

void ItemAPI::advancedPriceCheck(std::shared_ptr<PItem> item)
{
    if (item->filters.empty() || item->f_type.category == "map")
    {
        // Cannot advanced search items with no filters
        emit humour(tr("Advanced search is unavailable for this item type"));
        return;
    }

    if (!item->f_misc.identified)
    {
        emit humour(tr("Advanced search is unavailable for unidentified items"));
        return;
    }

    StatDialog dlg(item.get());
    auto       result = dlg.exec();

    if (result == QDialog::Rejected)
    {
        // cancelled
        return;
    }

    bool searchonsite = (result == SEARCH_ON_SITE);

    json filters = dlg.filters;
    json misc    = dlg.misc;

    auto query = R"(
    {
        "query": {
            "status": {
                "option": "online"
            },
            "stats": [{
                "type": "and",
                "filters": []
            }]
        },
        "sort": {
            "price": "asc"
        }
    }
    )"_json;

    QSettings settings;
    auto&     qe = query["query"];

    bool        is_unique_base = false;
    std::string searchToken;

    if (!item->name.empty())
    {
        is_unique_base = m_uniques.contains(item->name);
        searchToken    = item->name;
    }
    else
    {
        is_unique_base = m_uniques.contains(item->type);
        searchToken    = item->type;
    }

    // Force rarity if unique
    if (item->f_type.rarity == "Unique")
    {
        std::string rarity = item->f_type.rarity;
        std::transform(rarity.begin(), rarity.end(), rarity.begin(), ::tolower);

        query["query"]["filters"]["type_filters"]["filters"]["rarity"]["option"] = rarity;
    }

    // Force category
    if (!item->f_type.category.empty())
    {
        std::string category = item->f_type.category;
        std::transform(category.begin(), category.end(), category.begin(), ::tolower);

        query["query"]["filters"]["type_filters"]["filters"]["category"]["option"] = category;
    }

    // TODO weapon/armour base mods

    // Checked mods
    for (auto& [k, e] : filters.items())
    {
        // set id
        e["id"] = k;

        if (e["disabled"] == false)
        {
            qe["stats"][0]["filters"].push_back(e);
        }
    }

    // Check for unique items
    if (is_unique_base)
    {
        auto range = m_uniques.equal_range(searchToken);
        for (auto it = range.first; it != range.second; ++it)
        {
            auto& entry = it->second;

            // For everything else, match type
            if (entry["type"] == item->type)
            {
                qe["type"] = entry["type"];

                if (entry.contains("name"))
                {
                    qe["name"] = entry["name"];
                }

                break;
            }
        }
    }

    item->m_options = getLeague().toStdString();

    // Use sockets
    if (misc.contains("use_sockets") && misc["use_sockets"])
    {
        qe["filters"]["socket_filters"]["filters"]["sockets"]["min"] = item->f_socket.sockets.total();

        item->m_options += ", " + std::to_string(item->f_socket.sockets.total()) + "S";
    }

    // Use links
    if (misc.contains("use_links") && misc["use_links"])
    {
        qe["filters"]["socket_filters"]["filters"]["links"]["min"] = item->f_socket.links;

        item->m_options += ", " + std::to_string(item->f_socket.links) + "L";
    }

    // Use iLvl
    if (misc.contains("use_ilvl") && misc["use_ilvl"])
    {
        qe["filters"]["misc_filters"]["filters"]["ilvl"]["min"] = misc["ilvl"];

        item->m_options += ", iLvl=" + std::to_string(misc["ilvl"].get<int>());
    }

    // Use item base
    if (misc.contains("use_item_base") && misc["use_item_base"])
    {
        qe["type"] = item->type;

        item->m_options += ", Use Base Type";
    }

    // Shaper
    if (misc.contains("use_shaper_base") && misc["use_shaper_base"])
    {
        qe["filters"]["misc_filters"]["filters"]["shaper_item"]["option"] = true;
        item->m_options += ", Shaper Base";
    }

    // Elder
    if (misc.contains("use_elder_base") && misc["use_elder_base"])
    {
        qe["filters"]["misc_filters"]["filters"]["elder_item"]["option"] = true;
        item->m_options += ", Elder Base";
    }

    // Default corrupt options
    bool corrupt_override = settings.value(PTA_CONFIG_CORRUPTOVERRIDE, PTA_CONFIG_DEFAULT_CORRUPTOVERRIDE).toBool();

    if (corrupt_override)
    {
        QString corrupt_search = settings.value(PTA_CONFIG_CORRUPTSEARCH, PTA_CONFIG_DEFAULT_CORRUPTSEARCH).toString();

        if (corrupt_search != "Any")
        {
            qe["filters"]["misc_filters"]["filters"]["corrupted"]["option"] = (corrupt_search == "Yes");

            item->m_options += ", Corrupted=" + corrupt_search.toStdString();
        }
        else
        {
            item->m_options += ", Corrupted=Any";
        }
    }
    else
    {
        qe["filters"]["misc_filters"]["filters"]["corrupted"]["option"] = item->f_misc.corrupted;

        item->m_options += ", Corrupted=";
        item->m_options += item->f_misc.corrupted ? "Yes" : "No";
    }

    auto qba = query.dump();

    QNetworkRequest request;
    request.setUrl(QUrl(tradeSearchUrl + getLeague()));
    request.setRawHeader("Content-Type", "application/json");

    auto req = m_manager->post(request, QByteArray::fromStdString(qba));
    connect(req, &QNetworkReply::finished, [=]() {
        req->deleteLater();

        if (req->error() != QNetworkReply::NoError)
        {
            qWarning() << "PAPI: Error querying trade site" << req->error() << req->errorString();
            return;
        }

        auto respdata = req->readAll();
        auto resp     = json::parse(respdata.toStdString());
        if (!resp.contains("result") || !resp.contains("id"))
        {
            emit humour(tr("Error querying trade site. See log for details"));
            qWarning() << "PAPI: Error querying trade site";
            qWarning() << "PAPI: Site responded with" << respdata;
            return;
        }

        if (resp["result"].size() == 0)
        {
            emit humour(tr("No results found."));
            qDebug() << "No results";
            return;
        }

        if (searchonsite)
        {
            QDesktopServices::openUrl(QUrl(tradeSiteUrl + getLeague() + "/" + QString::fromStdString(resp["id"].get<std::string>())));
        }
        else
        {
            // else process the results
            processPriceResults(item, resp);
        }
    });
}
