# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds mod/OnlineCoop/entities/coop/coop_partner.ent, the other player's
# body: the mesh of the living French soldier (soldier_alive, head included)
# driven by the player's own Body-layer animations (idle, walk in four
# directions, run, crouch).
#
# The animations must be private copies, never the player's files: the
# engine shares an animation resource between every entity that loads it,
# and the soldier skeleton differs slightly from the player's (bone order,
# a few missing bones). Sharing broke both bodies in game: the real player's
# aiming and moves, and the avatar's legs. So the entity points to
# entities/coop/anims/coop_partner_<file>, and anims.txt lists which game
# files the installers copy there from the player's own installation (no
# game asset is redistributed with the mod).
#
# Entity settings follow lambert.ent (a Prop_Rigid with animations that the
# game spawns and animates from script).
#
#   python tools/MakePartnerEntity.py ["F:/.../Amnesia The Bunker"]
import os, re, sys

GAME = sys.argv[1] if len(sys.argv) > 1 else r'F:\SteamLibrary\steamapps\common\Amnesia The Bunker'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COOP = os.path.join(ROOT, 'mod', 'OnlineCoop', 'entities', 'coop')
ANIM_DIR = 'entities/coop/anims'
ANIMS = ['idle_stand', 'walk_fwd', 'walk_back', 'walk_left', 'walk_right', 'run_fwd',
         'crouch_idle', 'crouch_fwd', 'crouch_back', 'crouch_left', 'crouch_right']

def read(rel):
    with open(os.path.join(GAME, rel), encoding='utf-8') as f:
        return f.read()

lambert = read('entities/character/npc/lambert/lambert.ent')
soldier = read('entities/character/npc/soldier_alive/soldier_alive.ent')
player = read('entities/character/player/player_bunker.ent')

mesh = re.search(r'<Mesh .*?</Mesh>', soldier, re.S).group(0)
anims, copies = [], []
for name in ANIMS:
    m = re.search(r'<Animation Name="%s" File="([^"]+)" Layer="([^"]+)"' % re.escape(name), player)
    if not m:
        sys.exit('animation %s not found in player_bunker.ent' % name)
    # Body layer only: the LeftArm/RightArm layers are masked by the engine
    # for the player body alone and drive the whole skeleton elsewhere
    # (tested: hunched, turned-around pose); the arms get a script pose.
    if m.group(2) != 'Body':
        sys.exit('%s is on layer %s, expected Body' % (name, m.group(2)))
    stem = os.path.splitext(m.group(1))[0]   # entities/.../walk_bkwd
    private = '%s/coop_partner_%s' % (ANIM_DIR, os.path.basename(stem))
    for ext in ('.anm', '.fbx'):
        if os.path.exists(os.path.join(GAME, stem + ext)):
            copies.append('%s%s|%s%s' % (stem, ext, private, ext))
    # Footstep and foot-IK events belong to the real player: left out.
    anims.append('            <Animation Name="%s" File="%s.fbx" Layer="Default" Speed="1" />' % (name, private))

variables = re.search(r'<UserDefinedVariables EntityType="Prop_Rigid">.*?</UserDefinedVariables>', lambert, re.S).group(0)
variables = variables.replace('Name="VoiceSourceBone" Value="j_Head_02_Head"', 'Name="VoiceSourceBone" Value="j_Head"')
variables = variables.replace('Name="RandomizeAnimationStart" Value="true"', 'Name="RandomizeAnimationStart" Value="false"')

ent = ('<Entity>\n    <ModelData>\n        <Entities />\n        ' + mesh + '\n        <Bones />\n        <Shapes />\n'
       '        <Bodies />\n        <Joints />\n        <Animations>\n' + '\n'.join(anims) +
       '\n        </Animations>\n        <ProcAnimations />\n    </ModelData>\n    ' + variables + '\n</Entity>\n')
os.makedirs(COOP, exist_ok=True)
with open(os.path.join(COOP, 'coop_partner.ent'), 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(ent)
with open(os.path.join(COOP, 'anims.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
    f.write('# game file|mod file, copied by InstallMod.ps1 and the launcher\n' + '\n'.join(copies) + '\n')
print('wrote coop_partner.ent with %d animations, anims.txt with %d files' % (len(anims), len(copies)))
