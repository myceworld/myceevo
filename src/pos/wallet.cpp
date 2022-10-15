#include <pos/wallet.h>

wallet::CWallet* CStakeWallet::GetStakingWallet()
{
    if (!ready) {
        return nullptr;
    }

    return wallet;
}
