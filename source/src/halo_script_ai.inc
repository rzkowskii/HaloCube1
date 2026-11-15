// AI, unit and object-state script commands. Included by halo_script.cpp inside
// its anonymous namespace, after the Scenario class: ai_* flag setters and
// migration, unit_* and object_* state, damage, vitality, hover, doors.
//
// Commands that need systems this engine does not have (vehicle seating for
// actors, dormancy, emotions, dialogue triggers) are accepted and recorded so
// the scripts that gate on them keep moving; see docs/macos-build.md.

    Actor *Scenario::scripted(int id)
    {
        return id >= 0 && size_t(id) < actors.size() ? &actors[id] : nullptr;
    }

    SquadBot *Scenario::squadBotOf(const playerent *unit)
    {
        for(SquadBot &bot : squadBots) if(bot.bot == unit) return &bot;
        return nullptr;
    }

    // A unit value is the player, one actor, or a list of actors (ai_actors, players).
    template<class F> void Scenario::eachUnit(int value, F f)
    {
        if(value == playerObject || value == playerList) { f(player1); return; }
        if(value <= actorList)
        {
            const int reference = at(actorLists, actorList - value) ? actorLists[actorList - value] : -1;
            eachActor(reference, [&](SquadBot &bot, int) { f(bot.bot); });
            return;
        }
        if(playerent *unit = actorFromValue(value)) if(unit != player1) f(unit);
    }

    // Flags are remembered on the squad entry so a revert, which respawns the actors, restores them.
    void Scenario::setUnitFlag(playerent *unit, int flag, bool on)
    {
        if(!unit || unit == player1) return;
        haloactorsetflag(unit, flag, on);
        if(SquadBot *bot = squadBotOf(unit)) bot->scriptFlags = on ? bot->scriptFlags | flag : bot->scriptFlags & ~flag;
    }

    vec Scenario::startPoint(const SquadStart &start) const
    {
        const Projection &p = *projection;
        return vec((start.point.x - p.origin.x) * p.xy + p.margin, (p.origin.y - start.point.y) * p.xy + p.margin,
                   (start.point.z - p.origin.z) * p.z);
    }

    // Moves a living actor into another squad (ai_migrate and relatives): it takes that squad's positions and walks to them.
    void Scenario::migrateBot(SquadBot &bot, int newGroup)
    {
        bot.encounter = newGroup & 0xffff;
        const unsigned kind = uint32_t(newGroup) >> 30;
        if(kind == 2) bot.squad = (uint32_t(newGroup) >> 16) & 0xff;
        const Encounter *encounter = at(program.encounters, bot.encounter);
        const Squad *squad = encounter ? at(encounter->squads, bot.squad) : nullptr;
        static const unsigned anyGroup[7] = {};
        if(bot.bot && bot.encounter < int(encounterPositions.size()))
            haloactorsetpositions(bot.bot, &encounterPositions[bot.encounter], squad ? squad->masks : anyGroup);
    }

    // ai_link_activation: once any actor of the first encounter is alert, the second one is woken too.
    void Scenario::updateLinks()
    {
        for(const auto &link : links)
        {
            bool active = false;
            eachActor(link.first, [&](SquadBot &bot, int) { active |= haloactorstatus(bot.bot) >= 4; });
            if(!active) continue;
            eachActor(link.second, [&](SquadBot &bot, int) { if(haloactorstatus(bot.bot) < 4) haloactorsetflag(bot.bot, HALO_AI_FORCE_ACTIVE, true); });
        }
    }

    bool Scenario::aiCommand(const char *name, const std::vector<Value> &args, bool scene, Value &out)
    {
        auto is = [name](const char *candidate) { return !strcmp(name, candidate); };
        auto value = [&](size_t i, double fallback = 0) { return i < args.size() ? args[i].number : fallback; };
        auto index = [&](size_t i) { return i < args.size() ? args[i].integer() : -1; };
        auto text = [&](size_t i) { return i < args.size() ? args[i].text : ""; };
        auto truth = [&](size_t i) { return i < args.size() && args[i].truth(); };
        auto answer = [&](double v) { out = number(v, 8); return true; };
        auto handled = [&]() { out = {}; return true; };
        auto object = [&](size_t i)
        {
            const int id = named(program.objects, text(i));
            return id >= 0 ? id : index(i);
        };
        // Whether a unit value names live actors (or the player) rather than a scripted object.
        auto isUnit = [&](int v) { return v == playerObject || v == playerList || v <= actorList || actorFromValue(v) != nullptr; };

        // The players are objects, not calls with arguments of their own; the second seat is empty.
        if(is("player0")) return answer(playerObject);
        if(is("player1")) return answer(noObject);

        // --- Encounter switches and orders -------------------------------------
        if(is("ai_berserk"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { haloactorberserk(bot.bot, truth(1)); });
            return handled();
        }
        if(is("ai_grenades")) { haloaisetgrenades(truth(0)); return handled(); }
        if(is("ai_allow_dormant") || is("ai_maneuver_enable"))
        {
            const int flag = is("ai_allow_dormant") ? HALO_AI_NO_DORMANT : HALO_AI_NO_MANEUVER;
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { setUnitFlag(bot.bot, flag, !truth(1)); });
            return handled();
        }
        if(is("ai_try_to_fight_nothing"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int)
            {
                setUnitFlag(bot.bot, HALO_AI_MAGIC_SIGHT, false);
                setUnitFlag(bot.bot, HALO_AI_FORCE_ACTIVE, false);
            });
            return handled();
        }
        if(is("ai_braindead_by_unit"))
        {
            if(!scene) eachUnit(index(0), [&](playerent *unit) { setUnitFlag(unit, HALO_AI_BRAINDEAD, truth(1)); });
            return handled();
        }
        if(is("ai_renew"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { haloactorrenew(bot.bot); });
            return handled();
        }
        if(is("ai_teleport_to_starting_location") || is("ai_teleport_to_starting_location_if_unsupported"))
        {
            const bool onlyUnsupported = is("ai_teleport_to_starting_location_if_unsupported");
            if(!scene && projection) eachActor(index(0), [&](SquadBot &bot, int)
            {
                if(onlyUnsupported && bot.bot->onfloor) return;
                if(haloenemymove(bot.bot, startPoint(*bot.start)))
                {
                    bot.bot->yaw = static_cast<botent *>(bot.bot)->targetyaw = 90 - bot.start->yaw / RAD;
                    haloactorsetpositions(bot.bot, bot.encounter < int(encounterPositions.size()) ? &encounterPositions[bot.encounter] : nullptr,
                                          at(program.encounters, bot.encounter) && at(program.encounters[bot.encounter].squads, bot.squad) ?
                                              program.encounters[bot.encounter].squads[bot.squad].masks : nullptr);
                }
            });
            return handled();
        }
        if(is("ai_prefer_target"))
        {
            if(!scene) eachUnit(index(0), [&](playerent *unit) { haloactorprefer(unit, truth(1)); });
            return handled();
        }
        if(is("ai_follow_target_ai"))
        {
            // The first living actor of the second group is the one to trail.
            playerent *leader = nullptr;
            eachActor(index(1), [&](SquadBot &bot, int) { if(!leader) leader = bot.bot; });
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { if(bot.bot != leader) haloactorfollow(bot.bot, leader); });
            return handled();
        }
        if(is("ai_follow_target_disable"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int)
            {
                haloactorfollow(bot.bot, nullptr);
                setUnitFlag(bot.bot, HALO_AI_FOLLOW_PLAYER, false);
            });
            return handled();
        }
        if(is("ai_follow_distance"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { haloactorfollowdistance(bot.bot, float(value(1))); });
            return handled();
        }
        if(is("ai_link_activation"))
        {
            const std::pair<int, int> link(index(0), index(1));
            if(std::find(links.begin(), links.end(), link) == links.end()) links.push_back(link);
            return handled();
        }
        if(is("ai_automatic_migration_target"))
        {
            if(truth(1)) migrationTargets.insert(index(0)); else migrationTargets.erase(index(0));
            return handled();
        }
        if(is("ai_free") || is("ai_free_units"))
        {
            // Freed actors leave their encounter's orders and fight on their own.
            auto release = [&](playerent *unit) { haloactorcommand(unit, nullptr); haloactorfollow(unit, nullptr); };
            if(!scene)
            {
                if(is("ai_free")) eachActor(index(0), [&](SquadBot &bot, int) { release(bot.bot); });
                else eachUnit(index(0), release);
            }
            return handled();
        }

        // --- Migration, attaching and vehicles ---------------------------------
        if(is("ai_migrate_and_speak"))
        {
            // The spoken line is dialogue this engine does not play.
            const int oldGroup = index(0), newGroup = index(1);
            if(!scene && newGroup != -1) for(SquadBot &bot : squadBots)
                if(bot.bot && matchesSquad(oldGroup, bot.encounter, bot.squad)) migrateBot(bot, newGroup);
            return handled();
        }
        if(is("ai_migrate_by_unit") || is("ai_attach"))
        {
            // Actors move into the squad; scripted objects only remember which squad they belong to.
            const int target = index(1);
            if(target == -1) return handled();
            if(isUnit(index(0)))
            {
                if(!scene) eachUnit(index(0), [&](playerent *unit) { if(SquadBot *bot = squadBotOf(unit)) migrateBot(*bot, target); });
            }
            else if(Actor *unit = scripted(object(0))) unit->squadReference = target;
            return handled();
        }
        if(is("ai_attach_free") || is("ai_detach"))
        {
            // Free units and detached units answer to no squad; a detached actor stands idle.
            if(isUnit(index(0))) { if(is("ai_detach") && !scene) eachUnit(index(0), [&](playerent *unit) { setUnitFlag(unit, HALO_AI_BRAINDEAD, true); }); }
            else if(Actor *unit = scripted(object(0))) unit->squadReference = -1;
            return handled();
        }
        // Actors do not drive or board vehicles; these are accepted so the scripts that order it go on.
        if(is("ai_exit_vehicle") || is("ai_vehicle_enterable_actor_type") || is("ai_vehicle_enterable_disable") ||
           is("ai_reconnect") || is("ai_timer_expire") || is("ai_dialogue_triggers") || is("ai_allegiance_remove"))
            return handled();
        if(is("ai_allegiance_broken")) return answer(0);

        // --- Dialogue ------------------------------------------------------------
        if(is("ai_conversation_advance"))
        {
            for(Talking &entry : talking) if(entry.id == index(0)) entry.wake = tick;
            return handled();
        }
        if(is("ai_conversation_line"))
        {
            // The line being spoken, counted from 1; past the last line once it has finished.
            for(const Talking &entry : talking) if(entry.id == index(0)) return answer(entry.line);
            const Conversation *conversation = at(program.conversations, index(0));
            const int *status = at(conversationStatus, index(0));
            return answer(conversation && status && *status == 5 ? int(conversation->lines.size()) : 0);
        }

        // --- Units -------------------------------------------------------------
        if(is("unit_impervious") || is("object_cannot_take_damage") || is("object_can_take_damage"))
        {
            const bool on = is("object_can_take_damage") ? false : is("object_cannot_take_damage") ? true : truth(1);
            const int flag = is("unit_impervious") ? HALO_AI_IMPERVIOUS : HALO_AI_NO_DAMAGE;
            if(isUnit(index(0)))
            {
                eachUnit(index(0), [&](playerent *unit)
                {
                    if(unit == player1) haloplayerprotect(on);
                    else setUnitFlag(unit, flag, on);
                });
            }
            else if(Actor *item = scripted(object(0))) item->cannotTakeDamage = on;
            return handled();
        }
        if(is("unit_doesnt_drop_items"))
        {
            eachUnit(index(0), [&](playerent *unit) { setUnitFlag(unit, HALO_AI_NO_DROPS, true); });
            return handled();
        }
        if(is("unit_set_current_vitality") || is("units_set_current_vitality") || is("unit_set_maximum_vitality"))
        {
            const bool maximum = is("unit_set_maximum_vitality");
            const float health = float(value(1)), shield = float(value(2));
            eachUnit(index(0), [&](playerent *unit)
            {
                if(unit == player1) haloplayersetvitality(health, shield, maximum);
                // An actor's vitality is a fraction of what it spawned with; its maximum stays as authored.
                else if(!maximum)
                {
                    haloactorsethealthfraction(unit, health);
                    haloenemysetshield(unit, shield);
                }
            });
            return handled();
        }
        if(is("unit_suspended"))
        {
            const bool on = truth(1);
            if(isUnit(index(0))) eachUnit(index(0), [&](playerent *unit) { if(SquadBot *bot = squadBotOf(unit)) bot->suspended = on; });
            else if(Actor *item = scripted(object(0))) item->suspended = on;
            return handled();
        }
        if(is("unit_set_enterable_by_player"))
        {
            if(Actor *item = scripted(object(0))) item->enterable = truth(1);
            return handled();
        }
        if(is("unit_open") || is("unit_close"))
        {
            if(Actor *item = scripted(object(0))) item->open = is("unit_open");
            return handled();
        }
        if(is("vehicle_hover"))
        {
            if(Actor *item = scripted(object(0))) item->hovering = truth(1);
            return handled();
        }
        if(is("unit_set_emotion") || is("units_set_desired_flashlight_state")) return handled();

        // --- Objects -----------------------------------------------------------
        if(is("object_set_collideable"))
        {
            if(Actor *item = scripted(object(0))) item->collideable = truth(1);
            return handled();
        }
        if(is("object_set_melee_attack_inhibited") || is("object_set_ranged_attack_inhibited"))
        {
            if(Actor *item = scripted(object(0))) (is("object_set_melee_attack_inhibited") ? item->meleeInhibited : item->rangedInhibited) = truth(1);
            return handled();
        }
        if(is("object_set_permutation"))
        {
            const int id = object(0);
            if(Actor *item = scripted(id))
            {
                item->permutation = text(1);
                // A door set to its "off" permutation is gone: it stops drawing and blocking.
                if(!strncmp(text(1), "door_off", 8) && size_t(id) < objectDevices.size() && objectDevices[id] >= 0)
                    devices[objectDevices[id]].visible = false;
            }
            return handled();
        }

        // --- Damage ------------------------------------------------------------
        // The retail damage effects are jpt tags this engine does not carry by name. Effects that
        // are guaranteed to kill (names with guaranteed, doom, plummet, death) kill; the rest hurt a fixed amount.
        if(is("damage_object") || is("damage_new"))
        {
            if(scene) return handled();
            const std::string effect = text(0);
            auto lethal = [&](const char *word) { return effect.find(word) != std::string::npos; };
            const int damage = lethal("guaranteed") || lethal("doom") || lethal("plummet") || lethal("death") ? INT_MAX : 40;
            auto hurt = [&](playerent *unit) { if(unit && unit->state == CS_ALIVE) dodamage(damage, unit, unit == player1 ? unit : player1, GUN_GRENADE); };
            if(is("damage_object")) eachUnit(index(1), hurt);
            else
            {
                // damage_new strikes everything near a flag.
                const Flag *flag = at(program.flags, index(1));
                if(flag && projection)
                {
                    const vec centre = toWorld(*projection, flag->pose.position);
                    const float radius = 4.0f * haloscenarioscale();
                    if(player1->o.dist(centre) <= radius) hurt(player1);
                    for(SquadBot &bot : squadBots) if(bot.bot && bot.bot->o.dist(centre) <= radius) hurt(bot.bot);
                }
            }
            return handled();
        }
        return false;
    }

    // --- Vehicles the engine drives (halo_vehicle.cpp) ---------------------------------
    // The Warthogs and turrets placed in this section answer the boarding and seat commands for
    // their scenario objects. Everything else (Pelicans, dropships, Banshees, Ghosts, and the
    // scripted cargo carried by them) falls through to the stand-ins and the attach code.
    bool Scenario::vehicleCommand(const char *name, const std::vector<Value> &args, bool scene, Value &out)
    {
        if(strncmp(name, "vehicle_", 8) && strncmp(name, "unit_", 5) && strncmp(name, "ai_", 3)) return false;
        auto is = [name](const char *candidate) { return !strcmp(name, candidate); };
        auto value = [&](size_t i, double fallback = 0) { return i < args.size() ? args[i].number : fallback; };
        auto index = [&](size_t i) { return i < args.size() ? args[i].integer() : -1; };
        auto text = [&](size_t i) { return i < args.size() ? args[i].text : ""; };
        auto truth = [&](size_t i) { return i < args.size() && args[i].truth(); };
        auto answer = [&](double v) { out = number(v, 8); return true; };
        auto handled = [&]() { out = {}; return true; };
        auto object = [&](size_t i)
        {
            const int id = named(program.objects, text(i));
            return id >= 0 ? id : index(i);
        };
        // The player and the actors in a unit value.
        auto units = [&](int v)
        {
            std::vector<playerent *> list;
            eachUnit(v, [&](playerent *unit) { if(unit) list.push_back(unit); });
            return list;
        };
        if(is("vehicle_test_seat_list") || is("vehicle_test_seat"))
        {
            const int id = object(0);
            const Object *o = at(program.objects, id);
            // The Maw's escape Warthog stands in for the scenario Warthogs (halovehicleescapeseat):
            // trench_jeep_test asks for trench_jeep1..4 and asspain_1..3.
            const bool warthog = o && o->kind == 1;
            const std::vector<playerent *> list = units(index(2));
            bool inside = false;
            if(warthog) for(playerent *unit : list) inside |= halovehicleescapeseat(id, text(1), unit);
            if(!halovehiclebound(id))
            {
                // A Warthog or a turret that is not in this section: nobody (but the escape
                // Warthog's driver) is in it.
                if(o && (o->kind == 1 || o->kind == 6)) return answer(inside);
                return false;
            }
            for(playerent *unit : list) inside |= halovehicleseattest(id, text(1), unit);
            return answer(inside);
        }
        if(is("unit_enter_vehicle") || is("vehicle_load_magic"))
        {
            // (unit_enter_vehicle unit vehicle seat), (vehicle_load_magic vehicle seat units)
            const bool enter = is("unit_enter_vehicle");
            const int id = object(enter ? 1 : 0);
            if(!halovehiclebound(id)) return false;
            bool loaded = false;
            for(playerent *unit : units(index(enter ? 0 : 2))) loaded |= halovehicleload(id, text(enter ? 2 : 1), unit);
            return loaded ? answer(1) : false;
        }
        if(is("unit_exit_vehicle"))
        {
            bool released = false;
            for(playerent *unit : units(index(0))) released |= halovehicleunseat(unit);
            return released ? answer(1) : false;
        }
        if(is("vehicle_unload"))
        {
            const int id = object(0);
            return halovehiclebound(id) ? answer(halovehicleunload(id, text(1))) : false;
        }
        if(is("vehicle_riders"))
        {
            // Only the player can be named in a list of riders here (actors ride by squad).
            const int id = object(0);
            return halovehiclebound(id) ? answer(halovehicleseattest(id, "", player1) ? playerList : 0) : false;
        }
        if(is("vehicle_driver") || is("vehicle_gunner"))
        {
            const int id = object(0);
            if(!halovehiclebound(id)) return false;
            return answer(halovehicleseattest(id, is("vehicle_driver") ? "driver" : "gunner", player1) ? playerObject : noObject);
        }
        if(is("unit_in_vehicle"))
        {
            const std::vector<playerent *> list = units(index(0));
            if(list.empty()) return false;
            bool inside = false;
            for(playerent *unit : list) inside |= halovehicleinside(unit);
            return answer(inside);
        }
        if(is("unit_set_enterable_by_player"))
        {
            // Recorded on the object as well (aiCommand).
            if(halovehiclebound(object(0))) halovehicleenterable(object(0), truth(1));
            return false;
        }
        if(is("ai_vehicle_enterable_distance") || is("ai_vehicle_enterable_actor_type") || is("ai_vehicle_enterable_disable"))
        {
            const int id = object(0);
            if(!halovehiclebound(id)) return false;
            if(is("ai_vehicle_enterable_distance")) halovehicleaiboard(id, float(value(1)), -2, false);
            else if(is("ai_vehicle_enterable_disable")) halovehicleaiboard(id, -1, -2, true);
            else
            {
                static const char *const kinds[] = {"grunt", "elite", "jackal", "hunter"};
                int kind = -1;
                loopi(4) if(strstr(text(1), kinds[i])) kind = i;
                halovehicleaiboard(id, -1, kind, false);
            }
            return handled();
        }
        if(is("ai_go_to_vehicle"))
        {
            // (ai_go_to_vehicle squad vehicle seat)
            const int id = object(1);
            if(scene || !halovehiclebound(id)) return false;
            std::vector<playerent *> list;
            eachActor(index(0), [&](SquadBot &bot, int) { list.push_back(bot.bot); });
            halovehicleaigoto(id, text(2), list.data(), int(list.size()));
            return handled();
        }
        if(is("ai_exit_vehicle"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { halovehicleunseat(bot.bot); });
            return false;
        }
        return false;
    }
