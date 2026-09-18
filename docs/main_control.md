# Logique de contrôle de `src/main.cpp`

Le [graphe Graphviz DOT](./main_control.dot) décrit le comportement actuel de
[main.cpp](../src/main.cpp) : dix sous-modules, répartis en deux bras de cinq,
exécutent le même programme. SM1 fournit la synchronisation et la référence
`sin(angle)` ; chaque module décode cette référence, calcule les nombres
d'insertion, puis sa propre commande à partir des mesures communes.

## Lire et générer le graphe

Le graphe unique comporte cinq groupes : démarrage, tâche critique, RS485,
consensus, console et arrière-plan. Les rectangles représentent des actions,
les losanges des décisions, les flèches pleines l'exécution et les appels/retours,
les pointillées les signaux et données. L'orange signale les actions propres à SM1.
Les callbacks de réception et les tâches de fond ont leur propre chemin d'exécution.

Depuis la racine du dépôt, avec Graphviz installé :

```sh
dot -Tsvg docs/main_control.dot -o docs/main_control.svg
```

Graphviz n'est pas disponible dans l'environnement de préparation : le rendu SVG
n'a donc pas été généré ni vérifié visuellement.

## Démarrage et tâches

L'identifiant du module est déterminé par son UID matériel. Une carte inconnue
reçoit l'identifiant zéro : `setup_routine()` affiche un message puis retourne
avant de configurer le contrôle et la communication.

Pour une carte reconnue, l'initialisation configure la puissance, les capteurs,
les tâches et le RS485 à `SPEED_20M`. Des calibrations spécifiques sont appliquées
à SM1 et SM6. Les condensateurs côté bas sont déconnectés et les limites de duty
sont réglées à zéro et un. SM1 initialise la synchronisation maître et le scope ;
les autres modules initialisent la synchronisation esclave.

`main()` appelle cette initialisation puis retourne. Le fonctionnement continu
vient de la tâche critique toutes les **200 µs**, de la tâche console, de la tâche
d'arrière-plan et du callback `reception_function()`.

## Une ronde appliquée, la suivante préparée

Une *ronde* comprend la commande de SM1 et une mesure de chacun des dix modules.
Au tick qui consomme la ronde **k**, l'ordre de la tâche critique est le suivant :

1. Lire la tension locale et le courant local. `Arm_current` reçoit l'opposé de
   `I1_LOW` ; une lecture `NO_VALUE` conserve la mesure précédente.
2. Vérifier que les dix mesures de k sont présentes. Une ronde commencée mais
   incomplète produit `COMMUNICATION_ERROR`.
3. Consommer `requested_command`, le remettre à zéro, puis traiter `i`, `p` et les défauts.
4. Déterminer IDLE ou POWER. En POWER, décoder le sinus de k puis calculer et valider
   les nombres d'insertion. En cas d'échec, lever un défaut et revenir à IDLE ;
   sinon calculer les portes et appeler le scope sur SM1 à la cadence configurée.
5. Sur SM1, préparer `next_command` pour **k+1**, avec le numéro de cycle incrémenté
   et la prochaine référence sinus codée.
6. Effacer le compteur et les dix indicateurs de réception pour la prochaine fenêtre.
7. Forcer IDLE si un défaut subsiste, puis appliquer le duty ou arrêter le PWM.
8. Sur SM1, ouvrir k+1 et envoyer sa trame ; les autres transmissions suivent par RS485.

Les mesures locales lues au début du tick alimentent la nouvelle ronde. Le
classement des portes utilise les mesures partagées de la ronde précédente.
**SM1 décode et applique k comme les autres modules : il n'utilise ni un sinus
local non quantifié pour k, ni immédiatement la référence qu'il prépare pour k+1.**
Les échanges sont supposés terminés avant le tick suivant sur chaque carte ;
le code n'ajoute pas de temporisateur pour cette fenêtre.

### Condition initiale de POWER et validation NLM

```cpp
round_complete && cycle_command.status == POWER && !communication_fault &&
    (module_ID != MMC_SM1 || power_requested)
```

`round_complete` signifie exactement dix mesures enregistrées. Si cette condition
est fausse, le mode est IDLE. Les défauts sont des indicateurs séparés, pas un
troisième mode de fonctionnement.

Si cette condition donne POWER, `mmc_compute_insertion_counts()` décode la
référence et calcule la NLM. Il refuse -32768 et les nombres d'insertion arrondis
non finis ou hors de **[0, 5]**, avant toute conversion en `uint8_t`. Un échec
produit `COMMUNICATION_ERROR`, annule `power_requested` et remet le mode à IDLE.
Le consensus et l'acquisition du scope ne sont exécutés que si POWER subsiste.

En POWER, `LEG1` reçoit un duty de **1** si le module est sélectionné, sinon **0**.
Le PWM est démarré si nécessaire. En IDLE, le PWM précédemment actif est arrêté
avec `stop(ALL)` et `module_command` revient à zéro.

### Commandes et reprise après défaut

| Commande ou événement | Effet réel |
| --- | --- |
| `p` sur SM1, sans défaut au tick | Arme `power_requested` et prépare une ronde POWER. Le démarrage attend le tick suivant et une ronde complète valide. |
| `p` sur un esclave | Ignoré par la console. |
| `i` sur SM1 | Désarme POWER, arrête localement au tick de consommation et prépare IDLE pour la ronde suivante. |
| `i` sur un esclave | Désarme localement et lève `COMMUNICATION_ERROR`, reporté dans sa prochaine trame. |
| Défaut | Annule `power_requested` et impose IDLE au tick. SM1 prépare une ronde IDLE avec `sine_reference_raw = 0`. |
| Nouvelle ronde IDLE de SM1 | Efface le défaut de communication dans `begin_cycle()`, avant les autres validations. Une erreur de cette nouvelle ronde peut donc le rétablir. |

Un `p` consommé pendant qu'un défaut est actif est écarté. Après un défaut, une
**nouvelle commande `p` sur SM1** est nécessaire ; effacer le défaut ne réarme pas POWER.
Le premier `p` peut directement lancer une ronde POWER de collecte : il n'est pas
nécessaire qu'une ronde IDLE ait déjà été achevée.

L'arrêt des dix cartes n'est pas un événement instantané unique. Après `i` sur
SM1, les esclaves peuvent encore appliquer k avant de consommer la ronde IDLE
suivante. Après `i` sur un esclave, le défaut doit être transmis puis consommé par
les autres cartes ; si la chaîne ne termine pas, la ronde incomplète provoque
elle aussi un défaut au tick suivant.

## Communication RS485

La chaîne nominale est **SM1 → SM2 → … → SM10**. Chaque trame contient la référence
sinus codée, le numéro de cycle, la tension, le courant, le statut et l'identifiant
du module. Seule la référence de la trame SM1 sert de commande ; les suiveurs
recopient cette commande lorsqu'ils transmettent leurs propres mesures.

Le champ `int16_t sine_reference_raw` occupe les deux octets auparavant utilisés
par `n_insert_upper` et `n_insert_lower` : la taille totale de la trame reste
inchangée. Sa plage valide est **[-32767, 32767]** ; **-32768 est invalide** et
provoque un défaut de communication lors de la validation de la commande SM1.
Le protocole est incompatible avec les anciens firmwares malgré la même taille :
**les dix cartes doivent être mises à jour ensemble**.

SM1 ouvre la ronde après le contrôle. Un esclave ouvre sa ronde à réception de la
trame SM1, puis émet après acceptation de la trame de son prédécesseur. Chaque
module enregistre les mesures diffusées sur le bus. Avant son émission, il
enregistre également sa propre mesure : il ne dépend pas de son écho RS485.

`begin_cycle()` refuse une seconde ouverture si la collecte contient déjà des
mesures ; un numéro différent produit alors un défaut. Entre fenêtres, une trame
SM1 identique ou ancienne est ignorée. L'avance est calculée modulo 65536 ; sous
commande POWER elle doit valoir un. Un statut de commande supérieur à POWER ou une
référence sinus brute égale à -32768 produit aussi un défaut.

`store_module_measurements()` ignore les identifiants invalides, les doublons et
les mesures reçues avant celle de SM1. Un numéro de cycle différent produit un
défaut. Un statut supérieur à POWER devient le défaut local ; un désaccord entre
les statuts IDLE/POWER de la mesure et de la commande produit `COMMUNICATION_ERROR`.
Une émission locale reporte le défaut existant dans son champ `status`.

**Une trame acceptée et comptée n'est pas nécessairement saine** : les statuts en
défaut peuvent être enregistrés. La condition POWER vérifie donc à la fois la
complétude et l'absence de défaut. Avant toute première ronde, l'absence de trames
laisse le module en IDLE sans déclencher le contrôle de ronde incomplète.

## NLM et consensus des tensions

SM1 génère uniquement la référence sinus de la prochaine ronde, avec `f0 = 50 Hz`
et `Ts = 200 µs` :

```text
angle ← modulo_2π(angle + 2π × f0 × Ts)
next_command.sine_reference_raw ← mmc_encode_sine_reference(sin(angle))
```

La grandeur transportée est **`sin(angle)`**, sans multiplication par `m`.
L'encodeur renvoie -32768 si son entrée n'est pas finie. Sinon, il la sature sur
[-1, 1], multiplie par 32767 et arrondit avec `roundf`, ce qui produit un entier
dans [-32767, 32767].
En POWER, toutes les cartes, **SM1 compris**, décodent la commande de la ronde
achevée puis appliquent localement la NLM (*Nearest Level Modulation*) avec
`mmc_compute_insertion_counts()` :

```text
s  ← cycle_command.sine_reference_raw / 32767.0
Nu ← roundf(5 × (a + m × s) / 2)
Nl ← roundf(5 × (a − m × s) / 2)
```

Ces deux résultats doivent être finis et compris entre zéro et cinq pour que le
helper les convertisse en `uint8_t` et retourne `true`. Cette garde conserve la
vérification des bornes après le déplacement du calcul NLM sur chaque carte.

Les paramètres `a`, `m` et le nombre de modules par bras doivent être identiques
sur toutes les cartes ; actuellement `a = m = 1` et cinq modules par bras.
La même référence décodée assure le même calcul NLM, y compris sur SM1.
La quantification du sinus peut légèrement déplacer le franchissement d'un seuil
d'arrondi par rapport à l'ancien calcul effectué directement sur le sinus flottant.

Sans demande POWER, SM1 remet l'angle à zéro et prépare IDLE avec
`sine_reference_raw = 0`. **C'est le statut IDLE qui arrête les modules.**
Un sinus nul sous statut POWER reste une commande valide ; il donne ici `Nu = Nl = 3`.
Les deux arrondis sont indépendants : `Nu + Nl = 5` n'est pas un invariant,
notamment si les deux arguments valent exactement 2,5, arrondis chacun à 3.

Chaque carte en POWER recalcule les priorités des cinq modules de son propre bras,
puis retient uniquement sa porte. Les courants utilisés sont communs au bras :

```text
Bras supérieur SM1–SM5 : i_upper = MMC_arm_current[0] − 0,8 A
Bras inférieur SM6–SM10 : i_lower = MMC_arm_current[5] + 0,19 A
```

Chaque bras forme un anneau de voisins distinct. Pour le module i :

```text
e_i = (V_précédent + V_suivant) / 2 − V_i
e_i = 0 si |e_i| < 0,1 V
o_i = [V_i > V_précédent] + [V_i > V_suivant] − 1
p_i = 0,2 × e_i × d − 0,02 × o_i × d
```

Les crochets valent un si la comparaison est vraie, sinon zéro. Avec
`x = courant_du_bras / 1 A`, la direction lissée `d` vaut −1 pour `x ≤ −3`, +1
pour `x ≥ 3`, et `x × (27 + x²) / (27 + 9x²)` entre ces limites.

Le code sélectionne les `Nu` ou `Nl` plus grandes priorités. À égalité, le plus
petit indice gagne. Le courant positif favorise les tensions faibles par rapport
aux voisins ; le courant négatif inverse cette préférence. À courant nul, toutes
les priorités valent zéro et les premiers indices sont sélectionnés.

Ce « consensus » est un calcul instantané fondé sur les voisins, suivi d'un
classement des cinq priorités. Aucune convergence itérative n'est exécutée et les
portes ne sont pas envoyées sur le bus : les cartes les recalculent localement.

## Console, scope et limites de cette représentation

La console publie `p` et `i` dans une variable unique, consommée par la tâche
critique ; plusieurs saisies avant un tick peuvent donc se remplacer. `h` affiche
le menu, `a` bascule le déclencheur du scope et `r` demande son téléchargement.

Le scope est configuré uniquement sur SM1 : 1028 données, 14 canaux comprenant les
deux nombres d'insertion, dix tensions et deux courants. `scope.acquire()` est
appelé en POWER, à chaque tick avec `scope_period = 1` ; son déclencheur lit
`enable_acq`. La tâche d'arrière-plan de SM1 télécharge les données seulement en
IDLE. Elle éteint alors la LED ; en POWER elle la bascule, avec une suspension de
deux secondes entre passages. Les autres cartes n'exécutent pas ces actions.

Les constantes de surtension, sous-tension et surintensité ne constituent pas des
protections actives dans ce fichier : **aucun test électrique correspondant n'y
est exécuté**. Les seuils déclarés de 80 V et 8 A ne sont pas utilisés par la
boucle. Le graphe représente les vérifications présentes, sans ajouter de branche
de protection supposée ni de validation de fraîcheur des mesures conservées.

Le [README de `src`](../src/README.md) décrit une ancienne variante avec rampe de
duty et contrôleur central distinct. Le fichier actuel utilise SM1 comme maître,
dix modules et un duty directement égal à zéro ou un ; il fait référence ici.

### Parcours de référence et validation

| Parcours | Résultat attendu |
| --- | --- |
| UID inconnu | Retour avant création des tâches et configuration RS485. |
| Démarrage sans `p` | IDLE ; SM1 ouvre des rondes IDLE, PWM désactivé. Avant la première ronde, aucun défaut pour absence de trames. |
| `p` sur SM1 depuis IDLE | Collecte POWER, puis démarrage au tick suivant si la condition complète est satisfaite. |
| Ronde POWER complète | Décodage du sinus et NLM locale de k sur chaque carte, consensus, préparation de k+1 sur SM1, application PWM, puis échange. |
| Références brutes -32767, 0 et 32767 en POWER | Sinus décodés -1, 0 et 1 ; avec les paramètres actuels, `(Nu, Nl)` vaut `(0, 5)`, `(3, 3)` et `(5, 0)`. |
| Référence brute -32768 | Défaut de communication ; aucune entrée en POWER avec cette commande. |
| NLM arrondie non finie ou hors de [0, 5] | Échec du helper avant conversion, défaut de communication, demande POWER annulée et mode IDLE ; aucun consensus. |
| Ronde commencée mais incomplète | Défaut, IDLE, annulation de la demande POWER. |
| `i` sur SM1 ou sur un esclave | Arrêt local ; propagation par commande IDLE ou statut en défaut respectivement. |
| Défaut suivi d'IDLE | Effacement possible du défaut, sans réarmement automatique de POWER. |
| Tension au-delà du seuil déclaré | Aucune branche de protection électrique dans `main.cpp`. |

La compilation complète de l'environnement USB a réussi avec PlatformIO
(`python -m platformio run -e USB` depuis son environnement Python), sans
avertissement C++ sur `main.cpp` et sans téléversement.

Un banc hôte Clang++ a exécuté 20 fonctions extraites directement de `main.cpp`,
avec les interfaces matérielles simulées : **393 657 assertions réussies**.
Il couvre les 65 535 codes sinus valides, le code réservé, les valeurs non finies,
la saturation, les seuils d'arrondi, les paramètres de modulation locaux, le
transport des octets et les scénarios de contrôle/communication décrits ci-dessus
(démarrage différé, tours k/k+1, arrêts, défauts et reprise). Les dix identités de
module ont été exercées. L'ancienne et la nouvelle trame ont la même taille sur
l'hôte ; cette vérification ne constitue pas une validation de l'ABI ARM.

La structure et le sous-ensemble de syntaxe DOT utilisé ont aussi été vérifiés
avec `pyparsing` : cinq groupes, références de nœuds, branches étiquetées et liens
locaux du document. Cela ne remplace pas un rendu par Graphviz. Aucun essai sur
carte ni mesure du temps d'exécution réel n'a été effectué ; les fonctions
trigonométriques du banc hôte utilisaient les fonctions standard simulées.
