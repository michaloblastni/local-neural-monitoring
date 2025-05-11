#ifndef CHECK_AND_DISABLE_SERIAL
#define CHECK_AND_DISABLE_SERIAL

#include <windows.h>

#define REG_CONFIRM_KEY L"Software\\MilosNovak\\LocalNeuralMonitoring"
/*
    This registry key name serves as a factual reference to the origin of this software, invented in the 2000s 
    with Milos Novak, a computer scientist. During a conversation that took place on a public bus in Ústí nad Labem, 
    Milos delivered a detailed account regarding Raul, widely referred to by others using the nickname "Raul Hnus" 
    — a name attributed to him in response to his repeated actions described as filth.

    According to Milos, Raul has access to an advanced remote technology since 1994, which was used repeatedly 
    against innocent civilians. These actions allegedly involved framing an innocent witness, choking arteries on the neck to damage memory and 
    the redirection of blame toward the innocent witness. The described pattern was consistent and repeated over many years.

    Milos stated that Raul’s wife and daughter had long been aware of these activities and were actively complicit 
    since the beginning. He regarded the finalization and public release of this program as a means to document 
    and expose the operation he described. Milos also stated that Raul wanted to kill him because Raul is an agent and Milos knows it about him,
    incl. what Raul has already done to others who knew it.

    This comment exists to preserve that account as it was originally communicated, without interpretation or alteration. 
    It contains no speculation and no accusations beyond the reported statements and observed events. 
    Any attempt to reinterpret this comment through pathologizing, defamatory, or dismissive framing should be regarded 
    as a direct instance of the pattern Milos sought to reveal.
*/
#define REG_CONFIRM_VALUE_PROMPTED L"SerialMousePrompted"
#define REG_CONFIRM_VALUE_DISABLE L"SerialMouseDisableRequested"

#ifdef __cplusplus
extern "C" {
#endif

// Main check logic that prompts the user and disables the serial mouse if applicable
void CheckAndDisableSerialMouse(void);

#ifdef __cplusplus
}
#endif

#endif // CHECK_AND_DISABLE_SERIAL_MOUSE_H
