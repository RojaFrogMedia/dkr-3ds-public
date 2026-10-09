"""The test key makerom signs homebrew titles with.

This is not a secret and not Nintendo's: it is the "tpki" test key printed in
the source code of makerom (github.com/3DSGuy/Project_CTR, file
makerom/src/pki/test.h, MIT licence), which every homebrew .cia made with
makerom is signed with. A console only accepts such a title because custom
firmware does not check signatures. It is here so that the builder's .cia is
the same as one made by makerom.
"""

MODULUS = int(
    'cac588c7f12a092b7649c0a835751082c2b5e5b2e9c81888f39889bf9de6e40b'
    '715ddd3f138271f2ed318699d947fec57a7593e1f86dc63d9be11599e1c2e05c'
    '384b35a24d3ee2cefbb308a3dd0c2631849227c88a8ec883a86ca7a339719ef1'
    '349101df114a9cf98bf92f46440a7238f38b6d233389bf6634a786e6adf2def9'
    'ab16a140eed8f76cdc0092cb3149fc266424088fc660ff1ee3f0ddfb6d0d0f49'
    '7cad03ec9f6358fa46dfa2640ecc8557e72c617f59b8627d590ef684969942b0'
    '398380b5522e073f92e39ef547eba7d7d415f1228232be2ad08c01cc30a91196'
    'f6e92bea0ef82d0db191d51a9451b98539b0af9f549e99e146e56fe25f4b4e23', 16)

PRIVATE_EXPONENT = int(
    '3e2bbeba7f290252bf1bf1e4212fd9761e39234a6dff99f633aa2b62030a0e15'
    'ac16b9856377f5742461b1016eeb72241e5dfa8fa85a101447bd05a07ee5ff60'
    '872a1831c1396cd545bb290504fb7aa268215fed4efe646069bd96d0a7063d53'
    '7b6892885086ee065d72739a39b6723b200139df37281ef53963bc2af25eab1a'
    '99e45bebe636306c40016160cc55896dca7ee064787f7b26ae3ea3124516f6c8'
    'd0b94f91111211bbbb7fabc782dc4a619c14ae29fd3a601393192f5449b24434'
    '5814d72f7025a048667655879b25776d0b75988ba639403c217f2a24c1a5c1dc'
    '5a5754f603f6ad5133406d5c265e299282e529137d7dfe0873bc5dc4e92bd671', 16)
