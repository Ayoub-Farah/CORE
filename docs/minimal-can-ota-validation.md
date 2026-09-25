# Validation logicielle de l'OTA CAN minimale

Implémentation du plan du 25 septembre 2026, branche `feat/minimal-can-ota`.
Cette livraison sépare les classes `receiver` et `lead`, remplace le staging
Flash du Lead par une source PC bornée, et arme MCUboot seulement après une
intention de commit durable. La qualification matérielle n'est pas acquise.

## Vérifications exécutées

La suite `python -m unittest discover -s tests/ota -p '*.py'` passe **186 tests**.
Elle compile les sources C/C++ de production contre des interfaces de test et
exécute notamment deux campagnes successives du Lead, les barrières collectives,
les erreurs de transport, les retransmissions USB, la saturation de file,
les limites Flash, les journaux NVS et leur ramasse-miettes, l'activation différée,
les coupures simulées et la récupération précommit v2. Le codec USB est testé
entre le client Python et les vrais handlers C++/zcbor.

Les profils `OTA` et `USB_LEAD` produisent chacun une image USB signée distincte
et un artefact compact signé. La classe se trouve dans le TLV protégé `0xA0` ;
les outils et le stockage refusent une classe absente, dupliquée ou incompatible.
Le Lead n'est jamais une cible de la campagne récepteur. Aucun flash de matériel
n'a été effectué pendant cette implémentation.

Les six fichiers signés USB/CAN des builds démo récepteur, Lead et MMC de mesure
ont également été vérifiés avec `imgtool verify` et la clé existante. Trois
copies dont seule la signature a été altérée sont refusées, bien que leur hash
d'exécution reste cohérent. Cela vérifie la chaîne logicielle ; le comportement
du bootloader installé face à ces images reste à éprouver sur matériel.

`OTA_RECOVERY` v2 compile également pour ARM dans `.pio/rcv`, avec un journal
synthétique et sans toucher la configuration de récupération existante :
106 316 octets de Flash, 31 360 octets de RAM au lien, 106 652 octets utiles signés.
La vérification de l'entrée spécifique de récupération passe.

La référence MMC USB reproduit exactement **131 496 octets de Flash** et
**35 584 octets de RAM réservée** au linker. MMC_ANA est inchangé, SHA-256 :

```text
36B01F28C82D51DEA740A62825ECB60CC4F7AAB0753118B8B2077E18C2E9938D
```

## Résultat mémoire MMC

Mesure avec le chemin complet d'effacement/armement compilé dans la copie isolée :

| Ressource, en octets | MMC USB | MMC + récepteur minimal | Ajout OTA |
|---|---:|---:|---:|
| Flash au linker | 131 496 | 203 980 | **72 484** |
| RAM réservée au linker | 35 584 | 62 296 | **26 712** |
| RAM connue, Scope inclus | 93 264 | 119 976 | **26 712** |

L'artefact compact signé mesure **204 332 octets**, soit **16 852 octets** sous
la borne provisoire de 221 184 octets. La map réceptrice ne contient ni
coordinateur, ni runtime Lead, ni tableaux `inventory`/`frozen`. Sa configuration
effective désactive MCUmgr, le mode texte et le client ThingSet, utilise une file
de 3 éléments et une pile worker de 4 096 octets. Cette pile reste un réglage
provisoire dont le point haut doit être mesuré.

Le surcoût Flash respecte la cible de 83 000 octets ; la RAM est sous le plafond
de travail de 28 000 octets, au-dessus de la cible préférée de 25 000 octets.
La marge restante est **10 072 octets avant les autres allocations et les frais
du tas**. Pour conserver les 8 192 octets réels demandés, ces coûts inconnus ne
doivent pas dépasser **1 880 octets** : cette condition n'est pas encore vérifiée.
Le budget dynamique n'est donc pas déclaré accepté.

Les preuves locales sont archivées dans `ota-artifacts/qualification/` :
`mmc-minimal/sizes.json`, maps, configurations et artefact signé de mesure,
`signature-verification.json`, ainsi que les journaux de compilation et de tests.
Ces données sont ignorées par Git et doivent être conservées hors nettoyage.

## Reproduire le dimensionnement

Créer une copie isolée, en conservant un chemin court pour les outils Windows :

```sh
python owntech/tools/prepare_mmc_ota_sizing.py --mmc-source ../MMC/MMC_ANA/src/main.cpp --workspace .pio/mms
pio run -d .pio/mms -e USB -e OTA
python owntech/tools/ota_size_report.py --baseline-build .pio/mms/.pio/build/USB --receiver-build .pio/mms/.pio/build/OTA --mmc-source ../MMC/MMC_ANA/src/main.cpp --copied-source .pio/mms/src/main.cpp --archive ota-artifacts/qualification/mmc-minimal
```

Un répertoire de mesure existant n'est pas écrasé. La copie active le code complet
d'effacement/armement **pour le dimensionnement uniquement** ; elle ne doit pas
être flashée. Les profils livrés gardent `OWNTECH_OTA_DEFERRED_ARM_QUALIFIED=n`.
Le rapport archive les maps, configurations, artefact compact et manifeste hors
des répertoires nettoyés par PlatformIO. Les tailles au linker incluent les
réservations absentes du résumé PlatformIO. Les 57 680 octets de Scope sont
comptés séparément ; le reste des allocations et les frais du tas sont inconnus.

## Limites d'acceptation

Les [points bloquants MMC](ota-mmc-safety-audit.md) sont identifiés dans le source :
état privé `pwm_enable`, absence de signal de fin d'initialisation/santé et
supervision RS485 pendant maintenance. Les callbacks faibles restent en refus ;
le lien réussi ne rend donc pas MMC OTA opérationnel.

La [qualification de l'armement différé](ota-deferred-arm-qualification.md)
décrit le bootloader étudié, les bornes Flash et les preuves encore nécessaires.
Il reste à mesurer le tas réel et les piles, HRTIM/RS485 au repos et sous trafic,
puis à réaliser les installations USB et deux cycles CAN avec coupures sur une
flotte représentative. Aucune garantie de reset simultané ou de transition
atomique de toute la flotte n'est annoncée.

Le [guide opérateur v2](minimal-can-ota.md) décrit les nouvelles commandes et la
récupération ciblée. Les guides historiques sont marqués comme documentation du
prototype v1.
