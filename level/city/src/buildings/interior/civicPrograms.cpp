// svx_city — voxel_city buildings/interior/civicPrograms.js.
#include "buildings/interior/civicPrograms.hpp"

namespace svx::city {

namespace {

// A spec built as civicPrograms.js writes them: a type, then its keys.
struct Spec : CorridorSpec {
  explicit Spec(const char* t) { type = t; }
  Spec& span_(double v) {
    span = v;
    return *this;
  }
  Spec& at_(const char* v) {
    at = v;
    return *this;
  }
  Spec& ext_(const char* v) {
    ext = v;
    return *this;
  }
  Spec& split_(double v) {
    split = v;
    return *this;
  }
  Spec& lobby_() {
    lobby = true;
    return *this;
  }
  Spec& fire_() {
    fire = true;
    return *this;
  }
};

CorridorFloor floor_of(std::vector<CorridorSpec> front, std::vector<CorridorSpec> back, std::vector<std::string> fill) {
  CorridorFloor f;
  f.front = std::move(front);
  f.back = std::move(back);
  f.fill = std::move(fill);
  return f;
}

std::vector<std::pair<std::string, HallProgram>> make_hall_programs() {
  std::vector<std::pair<std::string, HallProgram>> out;
  {
    HallProgram P;
    P.foyer = 3.5;
    P.foyer_side = {"office"};
    P.hall = "sales";
    P.hall_open = true;
    P.hall_door = 16;
    P.back = {"storage", "breakroom", "wc", "storage"};
    P.back_depth = 5;
    P.back_w = 6;
    P.dock = "rollup";
    out.emplace_back("supermarket", P);
  }
  {
    HallProgram P;
    P.foyer = 8;
    P.foyer_side = {"cloakroom", "wc"};
    P.hall = "auditorium";
    P.hall_no_windows = true;
    P.hall_classical = true;
    P.back = {"dressing", "dressing", "storage", "wc"};
    P.back_depth = 5;
    P.dock = "metal";
    P.upper_foyer = "foyerBar";
    P.upper_side = {"office"};
    out.emplace_back("concertHall", P);
  }
  {
    HallProgram P;
    P.foyer = 4;
    P.foyer_side = {"cloakroom", "wc"};
    P.hall = "venueFloor";
    P.hall_no_windows = true;
    P.back = {"dressing", "storage"};
    P.back_depth = 4;
    P.dock = "metal";
    out.emplace_back("musicClub", P);
  }
  {
    HallProgram P;
    P.foyer = 7;
    P.foyer_side = {"kiosk", "wc"};
    P.hall = "cinema";
    P.halls = {2, 3};
    P.hall_min_w = 9;
    P.hall_no_windows = true;
    P.back = {"storage", "wc"};
    P.back_depth = 3.5;
    P.dock = "metal";
    P.upper_foyer = "foyerBar";
    out.emplace_back("cinema", P);
  }
  {
    HallProgram P;
    P.foyer = 3.5;
    P.foyer_side = {"wc"};
    P.hall = "marketHall";
    P.hall_open = true;
    P.hall_door = 20;
    P.back = {"storage", "office"};
    P.back_depth = 4.5;
    P.dock = "rollup";
    out.emplace_back("marketHall", P);
  }
  {
    HallProgram P;
    P.foyer = 8;
    P.foyer_side = {"cloakroom", "wc"};
    P.hall = "auditorium";
    P.hall_no_windows = true;
    P.hall_classical = true;
    P.back = {"dressing", "clubroom", "storage"};
    P.back_depth = 5;
    P.dock = "metal";
    P.upper_foyer = "danceHall";
    P.upper_side = {"clubroom", "clubroom"};
    out.emplace_back("houseOfCulture", P);
  }
  return out;
}

CorridorProgram hospital() {
  CorridorProgram P;
  P.bay = 6.5;
  P.corr = 3;
  P.corr_paint = "PAINT_MINT";
  P.corr_floor = "FLOOR_LINOLEUM";
  P.ground = floor_of({Spec("lobby").at_("mid").lobby_(), Spec("waiting"), Spec("pharmacy"), Spec("exam")},
                      {Spec("emergency").span_(2).ext_("back").at_("start"), Spec("radiology"), Spec("restroom")}, {"exam", "office"});
  P.first = floor_of({Spec("nurses").at_("mid")}, {Spec("operating").span_(2).at_("start"), Spec("operating"), Spec("storage"), Spec("restroom")}, {"ward"});
  P.upper = floor_of({Spec("nurses").at_("mid")}, {Spec("restroom")}, {"ward"});
  return P;
}

// A polyclinic (outpatients): a registry desk, doctors' rooms and waiting corners on every floor,
// x-ray, no wards ({...HOSPITAL, ground, first: null, upper}).
CorridorProgram polyclinic() {
  CorridorProgram P = hospital();
  P.ground = floor_of({Spec("lobby").at_("mid").lobby_(), Spec("pharmacy"), Spec("waiting")}, {Spec("radiology").at_("start"), Spec("restroom"), Spec("registry")}, {"exam"});
  P.first = std::nullopt;
  P.upper = floor_of({Spec("waiting").at_("mid")}, {Spec("restroom"), Spec("office")}, {"exam"});
  return P;
}

std::vector<std::pair<std::string, CorridorProgram>> make_corridor_programs() {
  std::vector<std::pair<std::string, CorridorProgram>> out;
  out.emplace_back("hospital", hospital());
  out.emplace_back("polyclinic", polyclinic());
  {
    CorridorProgram P;
    P.bay = 4.5;
    P.corr = 2.5;
    P.ground = floor_of({Spec("policeDesk").at_("mid").lobby_().span_(2), Spec("interview"), Spec("interview")},
                        {Spec("cell").split_(3), Spec("cell").split_(2), Spec("lockerRoom"), Spec("garageBay").span_(2).ext_("rollupBack").at_("end")}, {"office"});
    P.upper = floor_of({Spec("briefing").span_(2).at_("mid")}, {Spec("evidence"), Spec("restroom")}, {"office"});
    out.emplace_back("policeStation", P);
  }
  {
    CorridorProgram P;
    P.bay = 5;
    P.corr = 2.5;
    P.ground = floor_of({Spec("garageBay").span_(3).ext_("rollupFront").at_("start").fire_(), Spec("reception").lobby_().at_("end")},
                        {Spec("lockerRoom"), Spec("storage"), Spec("restroom")}, {"office"});
    P.upper = floor_of({Spec("dorm"), Spec("dorm")}, {Spec("kitchen"), Spec("dining"), Spec("restroom")}, {"office"});
    out.emplace_back("fireStation", P);
  }
  {
    CorridorProgram P;
    P.bay = 8;
    P.corr = 3.5;
    P.corr_floor = "FLOOR_MARBLE";
    P.ground = floor_of({Spec("lobby").at_("mid").lobby_(), Spec("museumShop"), Spec("cloakroom")}, {Spec("exhibit").span_(2), Spec("exhibit").span_(2)}, {"exhibit"});
    P.upper = floor_of({Spec("exhibit").span_(2), Spec("cafe")}, {Spec("exhibit").span_(2), Spec("exhibit").span_(2)}, {"exhibit"});
    out.emplace_back("museum", P);
  }
  {
    CorridorProgram P;
    P.bay = 9;
    P.corr = 3.5;
    P.corr_floor = "FLOOR_OAK";
    P.corr_paint = "PAINT_WHITE";
    P.ground = floor_of({Spec("lobby").at_("mid").lobby_(), Spec("museumShop"), Spec("cafe")}, {Spec("gallery").span_(2)}, {"gallery"});
    P.upper = floor_of({Spec("gallery").span_(2)}, {Spec("gallery").span_(2)}, {"gallery"});
    out.emplace_back("artGallery", P);
  }
  {
    CorridorProgram P;
    P.bay = 7;
    P.ground = floor_of({Spec("reception").at_("mid").lobby_(), Spec("library"), Spec("study")}, {Spec("library").span_(2), Spec("restroom")}, {"library"});
    P.upper = floor_of({Spec("study"), Spec("meeting")}, {Spec("library").span_(2)}, {"library"});
    out.emplace_back("library", P);
  }
  {
    CorridorProgram P;
    P.bay = 5.5;
    P.corr_floor = "FLOOR_PARQUET";
    P.ground = floor_of({Spec("lobby").at_("mid").lobby_(), Spec("waiting"), Spec("registry")}, {Spec("registry"), Spec("office"), Spec("restroom")}, {"office"});
    P.first = floor_of({Spec("council").span_(3).at_("mid")}, {Spec("office"), Spec("meeting"), Spec("restroom")}, {"office"});
    P.upper = floor_of({}, {Spec("restroom")}, {"office"});
    out.emplace_back("townHall", P);
  }
  {
    CorridorProgram P;
    P.bay = 4.5;
    P.corr = 2;
    P.corr_floor = "FLOOR_CARPET_RED";
    P.ground = floor_of({Spec("reception").at_("mid").lobby_(), Spec("restaurant").span_(2), Spec("pub")}, {Spec("kitchen"), Spec("office"), Spec("restroom"), Spec("storage")}, {"pub"});
    P.upper = floor_of({}, {Spec("storage")}, {"hotelRoom"});
    out.emplace_back("hotel", P);
  }
  return out;
}

template <class T>
const T* find_program(const std::vector<std::pair<std::string, T>>& table, const std::string& id) {
  for (const auto& p : table)
    if (p.first == id) return &p.second;
  return nullptr;
}

}  // namespace

const std::vector<std::pair<std::string, HallProgram>>& hall_programs() {
  static const std::vector<std::pair<std::string, HallProgram>> table = make_hall_programs();
  return table;
}

const std::vector<std::pair<std::string, CorridorProgram>>& corridor_programs() {
  static const std::vector<std::pair<std::string, CorridorProgram>> table = make_corridor_programs();
  return table;
}

const std::vector<std::pair<std::string, KioskProgram>>& kiosk_programs() {
  static const std::vector<std::pair<std::string, KioskProgram>> table = {{"petrolStation", KioskProgram{"grocery"}}};
  return table;
}

bool has_civic_planner(const std::string& id) {
  return find_program(hall_programs(), id) || find_program(corridor_programs(), id) || find_program(kiosk_programs(), id) || id == "departmentStore";
}

bool run_civic_planner(const std::string& id, const Envelope& env, Rng& rng, PlanBuilder& pb) {
  // (CIVIC_PLANNERS: the hall programs', then the corridor programs', the kiosks', the department
  // store's - ids are unique across the tables)
  if (const HallProgram* P = find_program(hall_programs(), id)) {
    plan_hall(env, rng, pb, *P);
    return true;
  }
  if (const CorridorProgram* P = find_program(corridor_programs(), id)) {
    plan_corridor(env, rng, pb, *P);
    return true;
  }
  if (const KioskProgram* P = find_program(kiosk_programs(), id)) {
    plan_kiosk(env, rng, pb, *P);
    return true;
  }
  if (id == "departmentStore") {
    plan_department_store(env, rng, pb);
    return true;
  }
  return false;
}

}  // namespace svx::city
