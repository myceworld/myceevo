#include <pos/wallet.h>

class CStakeWallet;
CStakeWallet stakeWallet;

wallet::CWallet* CStakeWallet::GetStakingWallet()
{
    if (!ready) {
        return nullptr;
    }

    return wallet;
}
