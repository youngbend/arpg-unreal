import unreal
def log(m): unreal.log("[P3] {}".format(m))

tools = unreal.AssetToolsHelpers.get_asset_tools()
f = unreal.StateTreeFactory()
try:
    f.set_editor_property("schema", unreal.StateTreeAIComponentSchema)
    log("schema set on factory")
except Exception as e:
    log("factory schema set failed: {}".format(e))
    log("factory props: {}".format([p for p in dir(f) if 'schema' in p.lower()]))

unreal.EditorAssetLibrary.make_directory("/Game/ARPG/NPCs")
if unreal.EditorAssetLibrary.does_asset_exist("/Game/ARPG/NPCs/ST_Probe"):
    unreal.EditorAssetLibrary.delete_asset("/Game/ARPG/NPCs/ST_Probe")
st = tools.create_asset(asset_name="ST_Probe", package_path="/Game/ARPG/NPCs",
                        asset_class=unreal.StateTree, factory=f)
log("created: {}".format(st))
ed = st.get_editor_property("edit_data") if False else None
for prop in ("edit_data","editor_data","EditorData"):
    try:
        ed = st.get_editor_property(prop); log("editor data via '{}' -> {}".format(prop, ed)); break
    except Exception as e:
        log("  no '{}': {}".format(prop, e))
if ed:
    subs = ed.get_editor_property("sub_trees")
    log("sub_trees: {} entries".format(len(subs)))
    for s in subs:
        log("  state '{}' type={} tasks={} conds={}".format(s.get_editor_property('name'),
            s.get_editor_property('type'), len(s.get_editor_property('tasks')),
            len(s.get_editor_property('enter_conditions'))))
    node = unreal.StateTreeEditorNode()
    log("empty node export: {!r}".format(node.export_text()))
    ist = node.get_editor_property("node")
    log("node.node -> {} export={!r}".format(ist, ist.export_text()))
    try:
        ist.import_text('/Script/ARPGAI.ARPGStateTreeTask_Attack')
        log("import_text(path) -> {!r}".format(ist.export_text()))
    except Exception as e:
        log("import_text(path) failed: {}".format(e))
