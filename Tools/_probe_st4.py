import unreal
def log(m): unreal.log("[P4] {}".format(m))
tools = unreal.AssetToolsHelpers.get_asset_tools()
f = unreal.StateTreeFactory()
f.set_editor_property("state_tree_schema_class", unreal.StateTreeAIComponentSchema)
log("schema class set OK")
unreal.EditorAssetLibrary.make_directory("/Game/ARPG/NPCs")
if unreal.EditorAssetLibrary.does_asset_exist("/Game/ARPG/NPCs/ST_Probe"):
    unreal.EditorAssetLibrary.delete_asset("/Game/ARPG/NPCs/ST_Probe")
st = tools.create_asset(asset_name="ST_Probe", package_path="/Game/ARPG/NPCs",
                        asset_class=unreal.StateTree, factory=f)
log("created: {}".format(st))
ed = st.get_editor_property("editor_data")
log("editor_data: {}".format(ed))
subs = ed.get_editor_property("sub_trees")
log("sub_trees: {}".format(len(subs)))
root = subs[0]
log("root name={} type={} children={}".format(root.get_editor_property('name'),
    root.get_editor_property('type'), len(root.get_editor_property('children'))))
node = unreal.StateTreeEditorNode()
ist = node.get_editor_property("node")
log("empty node.node export: {!r}".format(ist.export_text()))
for candidate in ['/Script/ARPGAI.ARPGStateTreeTask_Attack',
                  '(ScriptStruct="/Script/ARPGAI.ARPGStateTreeTask_Attack")',
                  'ARPGStateTreeTask_Attack']:
    try:
        ist.import_text(candidate)
        log("OK import {!r} -> export {!r}".format(candidate, ist.export_text()))
        break
    except Exception as e:
        log("fail import {!r}: {}".format(candidate, e))
unreal.EditorAssetLibrary.delete_asset("/Game/ARPG/NPCs/ST_Probe")
