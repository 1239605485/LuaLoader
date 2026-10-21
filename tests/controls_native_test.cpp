// Host mock of the supplied Android metadata/API. This does not emulate a GPU
// or claim a device test: it checks registry/API, native menu and input routing.
#define __ANDROID__ 1
#define __aarch64__ 1
#define TEF_API_IMPL 1
#include "../src/controls.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
extern "C" {
#include "lualib.h"
}
using namespace lualoader::controls;
struct Object {
 std::map<std::string,std::array<unsigned char,16>> fields;
 std::vector<Object*> items;
 std::vector<int> ints;
 std::string text;
};
Object world,prof,styles,binding,config,style,bindings,gui,editor,edit,panel,setting,options,cm,cur,sb;
std::vector<Object*> cursors;
std::vector<std::string> labels;
H handle(const char* s){return const_cast<char*>(s);}
template<class T>void put(Object& obj,const char* name,T v){auto& data=obj.fields[name];std::memcpy(data.data(),&v,sizeof(v));}
void* pointer(H f,void* o){auto obj=static_cast<Object*>(o?o:&world);auto i=obj->fields.find(static_cast<char*>(f));return i==obj->fields.end()?nullptr:i->second.data();}
void set(H f,H o,void* data){std::string name=static_cast<char*>(f);auto obj=static_cast<Object*>(o?o:&world);std::memcpy(obj->fields[name].data(),data,name=="Action"?4:1);}
std::vector<std::unique_ptr<Object>> strings;
Object* make_string(const std::string& s){strings.push_back(std::make_unique<Object>());strings.back()->text=s;return strings.back().get();}
Vec adjust_mock(Vec v){return {v.x/2,v.y/2};}
void rect_mock(void*,Rect,Color){}
Vec text_mock(void*,void* s,Vec,Color,float,float,float,int){labels.push_back(static_cast<Object*>(s)->text);return {};}
void* method_pointer(H h){std::string n=static_cast<char*>(h);if(n=="adjust")return reinterpret_cast<void*>(adjust_mock);if(n=="rect")return reinterpret_cast<void*>(rect_mock);if(n=="text")return reinterpret_cast<void*>(text_mock);return nullptr;}
bool invoke_mock(H method,H obj,void* result,void** args){std::string name=static_cast<char*>(method);
 if(name=="ui_open"){*static_cast<bool*>(result)=false;return true;}
 if(name=="profile"){*static_cast<H*>(result)=&prof;return true;}
 if(name=="count"){*static_cast<int*>(result)=static_cast<int>(cursors.size());return true;}
 if(name=="at"){*static_cast<H*>(result)=cursors.at(*static_cast<int*>(args[0]));return true;}
 if(name=="width"||name=="height"){*static_cast<int*>(result)=name=="width"?1280:720;return true;}
 if(name=="undo"){state.snapshot();return true;}
 if(name=="update"){put(binding,"Action",*static_cast<int*>(args[0]));return true;}
 if(name=="close"){put(*static_cast<Object*>(obj),"display",false);popup_closed(obj,nullptr,nullptr,nullptr);return true;}
 return false;
}
void wire(){
 patchlib_field_get_pointer=pointer;patchlib_field_set_value=set;
 patchlib_method_invoke_args=invoke_mock;patchlib_method_get_pointer=method_pointer;
 patchlib_string_create=[](const char* s)->H{return make_string(s);};
 patchlib_string_cstr=[](H s){return ::strdup(static_cast<Object*>(s)->text.c_str());};
 patchlib_array_length=[](H h){auto a=static_cast<Object*>(h);return a->items.empty()?a->ints.size():a->items.size();};
 patchlib_array_at=[](H h,size_t i,void* v){auto a=static_cast<Object*>(h);if(!a->items.empty()){if(i>=a->items.size())return false;*static_cast<H*>(v)=a->items[i];}
  else{if(i>=a->ints.size())return false;*static_cast<int*>(v)=a->ints[i];}return true;};
 api.styles_instance=handle("styles");api.active_profile=handle("profile");api.profile_id=handle("Id");api.profile_controls=handle("Controls");
 api.binding_slot=handle("SlotId");api.binding_action=handle("Action");api.slot_style=handle("Style");api.slot_kind=handle("kind");api.slot_id=handle("SlotId");
 api.batch=handle("batch");api.menu=handle("menu");api.manager_instance=handle("manager");api.count=handle("count");api.at=handle("at");api.adjust=handle("adjust");
 api.cursor_id=handle("Id");api.cursor_pos=handle("Position");api.cursor_down=handle("Down");api.cursor_old=handle("WasDown");api.cursor_ignore=handle("Ignore");api.cursor_world=handle("IgnoreForWorld");
 api.gui_runtime=handle("runtime");api.ui_open=handle("ui_open");api.gui_active=handle("gui");api.gui_edit=handle("edit");api.edit_enabled=handle("Enabled");
 api.active_control=handle("active");api.active_slot=handle("slot");api.options=handle("options");api.action_binding=handle("action_binding");api.display_options=handle("display");
 api.width=handle("width");api.height=handle("height");api.add_undo=handle("undo");api.update_value=handle("update");api.close_options=handle("close");api.rect=handle("rect");api.text=handle("text");
 put(gui,"runtime",&editor);put(world,"styles",&styles);put(world,"gui",&gui);put(world,"batch",&sb);put(world,"manager",&cm);put(gui,"edit",&edit);put(edit,"Enabled",false);
 static Object profid,slotid;profid.text="custom1";slotid.text="slot-A";
 put(prof,"Id",&profid);bindings.items={&binding};put(prof,"Controls",&bindings);put(binding,"SlotId",&slotid);put(binding,"Action",unbound);
 put(config,"SlotId",&slotid);put(config,"Style",&style);put(style,"kind",1);
 put(world,"active",&binding);put(world,"slot",&config);
 put(setting,"options",&options);put(setting,"action_binding",1);put(setting,"display",true);
 put(cur,"Id",7);put(cur,"Down",true);put(cur,"WasDown",false);put(cur,"Ignore",false);put(cur,"IgnoreForWorld",false);
 cursors={&cur};ready=true;tick=10;
}
void lua(lua_State* L,const char* code){if(luaL_dostring(L,code)!=LUA_OK){std::cerr<<lua_tostring(L,-1)<<'\n';std::abort();}}
int main(){
 wire();lualoader::lua_mod_handle_t mod;mod.mod_id="one.mod";auto L=luaL_newstate();luaL_openlibs(L);lua_newtable(L);register_api(L,&mod);lua_setglobal(L,"mod");
 lua(L,"assert(mod.controls.available()); assert(mod.controls.register{id='skill',name='投骨'}=='skill'); assert(not mod.controls.down('skill'))");
 auto key=State::key(mod.mod_id,"skill");state.bindings["custom1"]["slot-A"]=key;
 // A safe carrier exists only during native drawing, never in saved data.
 callbacks[0]={runtime_prefix,runtime_postfix,true};pre_guard<0>(nullptr,nullptr,nullptr,nullptr);
 assert(read<int>(api.binding_action,&binding)==carrier&&custom_slot(&config)==key);
 stop("injected draw failure");post_guard<0>(nullptr,nullptr,nullptr,nullptr);
 assert(read<int>(api.binding_action,&binding)==unbound&&render_bindings.empty());ready=true;
 // Real-screen coordinates must be converted, and foreign fingers untouched.
 regions["test"]={{100,100,80,80},key,"custom1","slot-A",tick};put(cur,"Position",Vec{240,240});
 put(cur,"IgnoreForWorld",true); // Native UI picking marks this even for valid buttons.
 poll_postfix(nullptr,nullptr,nullptr,nullptr);assert(read<bool>(api.cursor_ignore,&cur)&&state.find(key)->held);
 H cur_arg=&cur;void* args[]={&cur_arg};assert(dispatch_prefix(nullptr,args,nullptr,nullptr));
 lua(L,"assert(mod.controls.pressed('skill')); assert(not mod.controls.pressed('skill')); assert(mod.controls.down('skill'))");
 poll_postfix(nullptr,nullptr,nullptr,nullptr);lua(L,"assert(not mod.controls.pressed('skill'))");
 put(cur,"Down",false);put(cur,"WasDown",true);poll_postfix(nullptr,nullptr,nullptr,nullptr);assert(!state.find(key)->held&&dispatch_prefix(nullptr,args,nullptr,nullptr));
 lua(L,"assert(mod.controls.released('skill')); assert(not mod.controls.released('skill'))");
 // Multi-touch off rejects a second finger on the same native slot.
 claims.clear();state.reset_input();regions["test"].multi=false;
 put(cur,"Down",true);put(cur,"WasDown",false);put(cur,"Ignore",false);
 Object second=cur;put(second,"Id",8);cursors={&cur,&second};
 poll_postfix(nullptr,nullptr,nullptr,nullptr);assert(state.find(key)->presses==1&&claims.at(8).cancelled);
 put(cur,"Down",false);put(cur,"WasDown",true);put(second,"WasDown",true);poll_postfix(nullptr,nullptr,nullptr,nullptr);
 assert(!state.find(key)->held);
 // Multi-touch on merges fingers: first release cannot end a remaining hold.
 claims.clear();state.reset_input();regions["test"].multi=true;
 for(auto touch:{&cur,&second}){put(*touch,"Down",true);put(*touch,"WasDown",false);put(*touch,"Ignore",false);}
 poll_postfix(nullptr,nullptr,nullptr,nullptr);assert(!claims.at(8).cancelled&&state.find(key)->presses==1);
 put(cur,"Down",false);put(cur,"WasDown",true);put(second,"WasDown",true);poll_postfix(nullptr,nullptr,nullptr,nullptr);
 assert(state.find(key)->held&&state.find(key)->releases==0);
 put(second,"Down",false);poll_postfix(nullptr,nullptr,nullptr,nullptr);assert(!state.find(key)->held&&state.find(key)->releases==1);
 cursors={&cur};
 // Native editor menu can bind a registered action without custom enum values.
 claims.clear();put(edit,"Enabled",true);options.ints.clear();state.bindings.clear();
 assert(picker_prefix(&setting,nullptr,nullptr,nullptr));assert(!rows.empty());
 auto row=rows.front();assert(row.kind==2&&row.key==key);
 put(cur,"Down",true);put(cur,"WasDown",false);put(cur,"Ignore",false);put(cur,"IgnoreForWorld",false);
 put(cur,"Position",Vec{static_cast<float>((row.rect.x+8)*2),static_cast<float>((row.rect.y+8)*2)});
 poll_postfix(nullptr,nullptr,nullptr,nullptr);put(cur,"Down",false);put(cur,"WasDown",true);poll_postfix(nullptr,nullptr,nullptr,nullptr);
 assert(selected&&selected->key==key);picker_prefix(&setting,nullptr,nullptr,nullptr);
 assert(!read<bool>(api.display_options,&setting)&&read<int>(api.binding_action,&binding)==unbound);
 assert(state.binding("custom1","slot-A")==key);
 // Native menu closure cancels a buffered click, even if another UI closes it.
 put(setting,"display",true);picker_prefix(&setting,nullptr,nullptr,nullptr);
 selected=rows.front();popup_closed(&setting,nullptr,nullptr,nullptr);
 assert(!selected&&!popup_owner&&rows.empty());
 // Persistence and ownership survive reload, without a live action firing.
 auto directory=std::filesystem::temp_directory_path()/("ll-controls-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directory(directory);auto expected=state.bindings;configure_storage(directory.string());state.bindings=expected;save();state.bindings.clear();configure_storage(directory.string());assert(state.binding("custom1","slot-A")==key);
 unregister_owner(&mod);assert(!state.find(key)&&!state.binding("custom1","slot-A").empty());
 lua_close(L);std::filesystem::remove_all(directory);
 std::cout<<"PASS native menu, temporary carrier restoration, touch mapping, Lua polling, persistence and unload\n";
}
