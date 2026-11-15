// AI, unit and object-state script commands. Included by halo_script.cpp inside
// its anonymous namespace, after the Scenario class: ai_* flag setters and
// migration, unit_* and object_* state, damage, vitality, hover, doors.
//
// Commands that need systems this engine does not have (vehicle seating for
// actors, dormancy, emotions, dialogue triggers) are accepted and recorded so
// the scripts that gate on them keep moving; see docs/macos-build.md.

    bool seatNamed(const Seat &seat, const char *text);     // below, with the dropships

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

        // Volume tests of the player (players, or list_get of it), the living actors of a list
        // (ai_actors), one actor (list_get of a list) or a scripted object. _all needs every one
        // of them inside (so an empty list of actors passes, as in Halo); the others any one.
        if(is("volume_test_object") || is("volume_test_objects") || is("volume_test_objects_all") || is("volume_test_objects_any"))
        {
            const int volume = index(0), value = *text(1) ? object(1) : index(1);
            int tested = 0, inside = 0;
            if(value == playerObject || value == playerList) { ++tested; inside += testVolume(volume); }
            else if(isUnit(value))
                eachUnit(value, [&](playerent *unit)
                {
                    if(!unit || unit->state != CS_ALIVE) return;
                    ++tested;
                    inside += unitInVolume(volume, unit);
                });
            else { ++tested; inside += testVolume(volume, value); }
            return answer(is("volume_test_objects_all") ? inside == tested : inside > 0);
        }

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
           is("ai_reconnect") || is("ai_timer_expire") || is("ai_dialogue_triggers"))
            return handled();

        // --- Allegiances ---------------------------------------------------------
        // Team literals carry the retail game team (1 player, 2 human, 3 covenant, 4 flood,
        // 5 sentinel); halo_ai.cpp keeps the allegiance matrix the actors' targeting reads.
        if(is("ai_allegiance") || is("ai_allegiance_remove"))
        {
            const int a = haloteamfromretail(index(0), HALO_TEAM_DEFAULT), b = haloteamfromretail(index(1), HALO_TEAM_DEFAULT);
            if(a != HALO_TEAM_DEFAULT && b != HALO_TEAM_DEFAULT) haloallegianceset(a, b, is("ai_allegiance"));
            return handled();
        }
        if(is("ai_allegiance_broken"))
        {
            const int a = haloteamfromretail(index(0), HALO_TEAM_DEFAULT), b = haloteamfromretail(index(1), HALO_TEAM_DEFAULT);
            return answer(a != HALO_TEAM_DEFAULT && b != HALO_TEAM_DEFAULT && haloallegiancebroken(a, b));
        }

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
            HaloScriptDamage scripted;
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
    // The vehicles placed in this section (Warthogs, turrets, Ghosts, Wraiths, Scorpions and
    // Banshees) answer the boarding and seat commands for their scenario objects. Dropships and
    // Pelicans carry the actors the scripts load into them (Cargo, below) and take the player
    // aboard once a script makes them enterable; for the rest (and the seat tests of actors in
    // them) the stand-ins and the attach code still answer.
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
                // A Pelican the script made enterable: whether the player boarded it, in a seat
                // of that name (a30's extraction waits for p-riderlf or p-riderrf).
                if(scriptVehicle(id) && actors[id].boardable && std::find(list.begin(), list.end(), player1) != list.end())
                {
                    const Seat *seat = at(o->seats, boardedSeat);
                    return answer(boardedVehicle == id && (!*text(1) || (seat && seatNamed(*seat, text(1)))));
                }
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
            if(!halovehiclebound(id))
            {
                // A dropship or Pelican: the actors ride at its seats until vehicle_unload.
                if(scene || !scriptVehicle(id)) return false;
                bool loaded = false;
                for(playerent *unit : units(index(enter ? 0 : 2))) if(unit != player1) loaded |= loadCargo(id, text(enter ? 2 : 1), unit, false);
                return loaded ? answer(1) : false;
            }
            bool loaded = false;
            for(playerent *unit : units(index(enter ? 0 : 2))) loaded |= halovehicleload(id, text(enter ? 2 : 1), unit);
            // ai_vehicle_encounter named the squad its pilot fights with (c40's Wraiths).
            const auto squad = vehicleSquads.find(id);
            playerent *driver = halovehicledriverof(id);
            if(loaded && squad != vehicleSquads.end() && driver && driver != player1)
                if(SquadBot *bot = squadBotOf(driver)) migrateBot(*bot, squad->second);
            return loaded ? answer(1) : false;
        }
        if(is("unit_exit_vehicle"))
        {
            bool released = false;
            for(playerent *unit : units(index(0)))
            {
                released |= halovehicleunseat(unit);
                // Out of a dropship or Pelican too.
                for(size_t i = 0; i < cargo.size(); ++i)
                    if(cargo[i].unit == unit && !scene) { releaseCargo(cargo[i]); cargo.erase(cargo.begin() + i); released = true; break; }
            }
            return released ? answer(1) : false;
        }
        if(is("vehicle_unload"))
        {
            const int id = object(0);
            if(halovehiclebound(id)) return answer(halovehicleunload(id, text(1)));
            // A dropship sets down the actors in the seats named (a30's cd-passengerl01 ...). The
            // attach code below still runs, so the objects carried at those seats (b40's Ghosts
            // under c_dropship_b) are let go by the same call (vehicle_unload returns nothing).
            if(!scene) unloadCargo(id, text(1));
            return false;
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
            else if(scriptVehicle(object(0))) actors[object(0)].boardable = truth(1);
            return false;
        }
        if(is("ai_vehicle_encounter"))
        {
            // (ai_vehicle_encounter vehicle squad): its pilot fights with that squad, now or once
            // a script loads one.
            const int id = object(0);
            if(scene || id < 0) return handled();
            vehicleSquads[id] = index(1);
            playerent *driver = halovehiclebound(id) ? halovehicledriverof(id) : nullptr;
            if(driver && driver != player1) if(SquadBot *bot = squadBotOf(driver)) migrateBot(*bot, index(1));
            return handled();
        }
        if(is("ai_going_to_vehicle"))
        {
            const int id = object(0);
            if(halovehiclebound(id)) return answer(halovehicleaigoing(id));
            int walking = 0;
            for(const Cargo &entry : cargo) walking += entry.vehicle == id && entry.walking;
            return answer(walking);
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
            if(!scene && !halovehiclebound(id) && scriptVehicle(id))
            {
                // A Pelican or dropship: one actor of the squad per free seat of that name walks
                // to it (a30's marines to their extraction Pelican) and rides once there.
                eachActor(index(0), [&](SquadBot &bot, int)
                {
                    for(const Cargo &entry : cargo) if(entry.unit == bot.bot) return;
                    if(freeSeat(id, text(2)) >= 0) loadCargo(id, text(2), bot.bot, true);
                });
                return handled();
            }
            if(scene || !halovehiclebound(id)) return false;
            std::vector<playerent *> list;
            eachActor(index(0), [&](SquadBot &bot, int) { list.push_back(bot.bot); });
            halovehicleaigoto(id, text(2), list.data(), int(list.size()));
            return handled();
        }
        if(is("ai_exit_vehicle"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int)
            {
                halovehicleunseat(bot.bot);
                for(size_t i = 0; i < cargo.size(); ++i)
                    if(cargo[i].unit == bot.bot) { releaseCargo(cargo[i]); cargo.erase(cargo.begin() + i); break; }
            });
            return false;
        }
        return false;
    }

    // --- Dropships and Pelicans -------------------------------------------------------
    // The scenario's vehicles the engine does not drive move on their recordings (and parents)
    // in the script (updateRecordings) and are drawn while they do (render). Actors loaded into
    // them ride at a seat marker, pinned there every frame, until vehicle_unload sets them down
    // (they drop to the floor), the vehicle is destroyed, or a revert respawns them. Actors told
    // to walk to one (ai_go_to_vehicle) ride once they reach it. The player boards one with E once
    // a script makes it enterable (a30's extraction Pelicans), rides at its seat, and E gets them
    // out again; game_won while boarded ends the section (halo.cpp).

    // Case-insensitive: whether a seat's name or marker holds `text` (Halo matches seat labels
    // by part: "passenger" loads every CD-passenger seat, "rider" every P-rider seat).
    bool seatNamed(const Seat &seat, const char *text)
    {
        if(!text || !*text) return true;
        auto holds = [text](const std::string &name)
        {
            std::string a = name, b = text;
            for(char &c : a) c = char(tolower(c));
            for(char &c : b) c = char(tolower(c));
            return a.find(b) != std::string::npos;
        };
        return holds(seat.name) || holds(seat.marker);
    }

    // Whether an actor pointer still names a living bot (cargo outlives nothing: a revert or a
    // removal leaves stale pointers behind).
    bool livingUnit(const playerent *unit)
    {
        if(!unit) return false;
        loopv(players) if(players[i] == unit) return unit->state == CS_ALIVE;
        return false;
    }

    // A unit vehicle object (kind 0 and up) the engine does not drive in this section.
    bool Scenario::scriptVehicle(int id) const
    {
        const Object *o = at(program.objects, id);
        return o && at(actors, id) && o->kind >= 0 && !halovehiclebound(id) && projection;
    }

    bool Scenario::seatWorld(int vehicle, int seat, vec &point) const
    {
        if(!projection || !at(actors, vehicle) || !at(program.objects, vehicle)) return false;
        const Object &object = program.objects[vehicle];
        Pose pose = currentActor(vehicle);
        vec local(0, 0, 0);
        if(const Seat *s = at(object.seats, seat))
            if(const Flag *marker = at(object.markers, named(object.markers, s->marker.c_str())))
                local = vec(marker->pose.position).mul(actors[vehicle].scale);
        pose.position.add(poseOffset(local, pose));
        point = toWorld(*projection, pose.position);
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    }

    // The first seat of that name nobody rides in or walks to (or -1). A vehicle without seats
    // in the table takes one rider at its middle (seat -1) per call.
    int Scenario::freeSeat(int vehicle, const char *seat) const
    {
        const Object *o = at(program.objects, vehicle);
        if(!o) return -1;
        for(size_t i = 0; i < o->seats.size(); ++i)
        {
            if(!seatNamed(o->seats[i], seat) || (boardedVehicle == vehicle && boardedSeat == int(i))) continue;
            bool taken = false;
            for(const Cargo &entry : cargo) taken |= entry.vehicle == vehicle && entry.seat == int(i);
            if(!taken) return int(i);
        }
        return -1;
    }

    bool Scenario::loadCargo(int vehicle, const char *seat, playerent *unit, bool walking)
    {
        if(!unit || unit == player1 || unit->state != CS_ALIVE || !scriptVehicle(vehicle)) return false;
        cargo.erase(std::remove_if(cargo.begin(), cargo.end(), [unit](const Cargo &entry) { return entry.unit == unit; }), cargo.end());
        const Object &o = program.objects[vehicle];
        int index = freeSeat(vehicle, seat);
        // No free seat of that name: share the first one that matches (more actors than seats).
        if(index < 0) for(size_t i = 0; i < o.seats.size() && index < 0; ++i) if(seatNamed(o.seats[i], seat)) index = int(i);
        cargo.push_back({vehicle, index, unit, walking, lastmillis + 30000});
        if(halo_scripttrace)
            conoutf("Halo cargo: %s %s %s (%s)", unit->name, walking ? "walks to" : "rides in", o.name.c_str(),
                    at(o.seats, index) ? o.seats[index].name.c_str() : "-");
        return true;
    }

    // Out of the vehicle: from its seat, a step outward, falling to the floor below.
    void Scenario::releaseCargo(Cargo &entry)
    {
        playerent *unit = entry.unit;
        if(!livingUnit(unit)) return;
        haloactorgoto(unit, nullptr);
        if(entry.walking) return;
        vec seat;
        if(!seatWorld(entry.vehicle, entry.seat, seat)) return;
        vec centre = toWorld(*projection, currentActor(entry.vehicle).position), out = vec(seat).sub(centre);
        out.z = 0;
        if(!out.iszero()) out.normalize().mul(unit->radius * 2 + .5f);
        unit->o = vec(seat).add(out).add(vec(0, 0, unit->eyeheight));
        unit->vel = vec(0, 0, 0);
        unit->resetinterp();
        if(halo_scripttrace) conoutf("Halo cargo: %s set down by %s", unit->name, program.objects[entry.vehicle].name.c_str());
    }

    int Scenario::unloadCargo(int vehicle, const char *seat)
    {
        const Object *o = at(program.objects, vehicle);
        if(!o) return 0;
        int released = 0;
        for(size_t i = 0; i < cargo.size();)
        {
            Cargo &entry = cargo[i];
            const Seat *s = at(o->seats, entry.seat);
            if(entry.vehicle != vehicle || (*seat && s && !seatNamed(*s, seat)) || (*seat && !s && entry.seat >= 0)) { ++i; continue; }
            releaseCargo(entry);
            if(!entry.walking) ++released;
            cargo.erase(cargo.begin() + i);
        }
        return released;
    }

    // Every frame: riders held at their seats, walkers seated on arrival, the boarded player
    // carried with the vehicle (held where it was last seen once it is gone).
    void Scenario::updateCargo()
    {
        for(size_t i = 0; i < cargo.size();)
        {
            Cargo &entry = cargo[i];
            const bool alive = livingUnit(entry.unit);
            const Actor *vehicle = at(actors, entry.vehicle);
            vec seat;
            if(!alive || !vehicle || !vehicle->visible || !seatWorld(entry.vehicle, entry.seat, seat) || (entry.walking && lastmillis > entry.until))
            {
                if(alive && vehicle) releaseCargo(entry);
                cargo.erase(cargo.begin() + i);
                continue;
            }
            playerent *unit = entry.unit;
            if(entry.walking)
            {
                // Within a few world units of its seat (the hull is in the way of the marker itself).
                const vec feet = vec(unit->o).sub(vec(0, 0, unit->eyeheight));
                if(feet.distxy(seat) < 4.0f * projection->xy && fabsf(feet.z - seat.z) < 4.0f * projection->xy)
                {
                    entry.walking = false;
                    haloactorgoto(unit, nullptr);
                    if(halo_scripttrace) conoutf("Halo cargo: %s boards %s", unit->name, program.objects[entry.vehicle].name.c_str());
                }
                else if(tick % 15 == 0) haloactorgoto(unit, &seat);
                if(entry.walking) { ++i; continue; }
            }
            unit->o = vec(seat).add(vec(0, 0, unit->eyeheight * .45f));
            unit->vel = vec(0, 0, 0);
            unit->onfloor = true;
            unit->resetinterp();
            ++i;
        }
        if(boardedVehicle != noObject)
        {
            vec seat;
            const Actor *vehicle = at(actors, boardedVehicle);
            if(vehicle && vehicle->visible && seatWorld(boardedVehicle, boardedSeat, seat)) boardedAt = seat;
            if(player1->state != CS_ALIVE) { boardedVehicle = noObject; boardedSeat = -1; return; }
            player1->o = vec(boardedAt).add(vec(0, 0, player1->eyeheight * .45f));
            player1->vel = vec(0, 0, 0);
            player1->onfloor = true;
            player1->timeinair = 0;
            player1->resetinterp();
        }
    }

    // The enterable Pelican whose rider seat is nearest the player, within reach of it: Halo's
    // player seats are the front riders (P-riderLF, P-riderRF), so those come first.
    int Scenario::boardCandidate(int &seat) const
    {
        seat = -1;
        if(!projection || player1->state != CS_ALIVE || halovehicleseated()) return -1;
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        const float reach = 4.5f * projection->xy;
        int best = -1;
        float bestScore = 1e9f;
        for(size_t id = 0; id < actors.size(); ++id)
        {
            const Actor &actor = actors[id];
            if(!actor.visible || !actor.boardable || actor.recording || !scriptVehicle(int(id))) continue;
            const Object &o = program.objects[id];
            for(size_t i = 0; i < o.seats.size(); ++i)
            {
                const Seat &candidate = o.seats[i];
                if(!seatNamed(candidate, "rider") && !seatNamed(candidate, "passenger")) continue;
                vec point;
                if(!seatWorld(int(id), int(i), point)) continue;
                const float distance = feet.distxy(point);
                if(distance > reach || fabsf(feet.z - point.z) > reach) continue;
                std::string name = candidate.name;
                for(char &c : name) c = char(tolower(c));
                const bool front = name.size() >= 2 && (!name.compare(name.size() - 2, 2, "lf") || !name.compare(name.size() - 2, 2, "rf"));
                const float score = distance - (front ? 1e6f : 0);
                if(score < bestScore) { bestScore = score; best = int(id); seat = int(i); }
            }
        }
        return best;
    }

    bool Scenario::board()
    {
        if(boardedVehicle != noObject) { unboard(false); return true; }
        int seat;
        const int id = boardCandidate(seat);
        if(id < 0) return false;
        boardedFrom = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        if(!seatWorld(id, seat, boardedAt)) return false;
        boardedVehicle = id;
        boardedSeat = seat;
        player1->attacking = false;
        player1->stopmoving();
        haloplayermoved("into a script vehicle seat", &boardedFrom);
        conoutf("Halo: player aboard %s (%s)", program.objects[id].name.c_str(), program.objects[id].seats[seat].name.c_str());
        return true;
    }

    // Out again: back where they got in (`keepPlace`: where the seat last was, the vehicle gone).
    void Scenario::unboard(bool keepPlace)
    {
        if(boardedVehicle == noObject) return;
        const vec feet = keepPlace ? boardedAt : boardedFrom;
        boardedVehicle = noObject;
        boardedSeat = -1;
        if(player1->state != CS_ALIVE) return;
        player1->o = vec(feet).add(vec(0, 0, player1->eyeheight));
        player1->vel = vec(0, 0, 0);
        player1->resetinterp();
        haloplayermoved("out of a script vehicle seat");
        conoutf("Halo: player out of the script vehicle");
    }

    void Scenario::clearCargo()
    {
        for(Cargo &entry : cargo) if(livingUnit(entry.unit)) haloactorgoto(entry.unit, nullptr);
        cargo.clear();
        boardedVehicle = noObject;
        boardedSeat = -1;
    }

