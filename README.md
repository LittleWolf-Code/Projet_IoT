a ajouter 

## 1) Web App
### Fréquence : Toutes les 30 secondes, on analyse les données de la minute précédente.( on peut aussi passer a 2 minutes)

### Cas 1 : Un seul ESP32 détecte le device, on considere que le divice est dans la piece de l'esp32 a la position de l'esp 32

### Cas 2 : Deux ESP32 détectent le device

Problème : L'intersection de deux cercles donne deux points possibles.

Si on a l'historique de la position précédente, on choisit le point le plus proche de la dernière position connue.

### Cas 3 : Trois ESP32 (ou plus) détectent le device

Action : Trilatération

### Cas 4 : plus de 3 ESP32, on prend les 3 plus proche

## 2). Gestion des Étages (Axe Z)

z= 0 RDC

z=1 etage 1 

### Règle de décision Z : L'étage ($z$) est défini par l'ESP32 qui possède le RSSI le plus fort (le plus proche).

### Filtrage : Pour le calcul $(x, y)$, on ne sélectionne que les ESP32 appartenant à cet étage $z$. Si des ESP d'un autre étage captent le signal, on les ignore pour le calcul de position 2D afin de ne pas fausser les distances.



## 3) Ajout base de donnée influxdB

   -stockage des scan BLe ( realiser par NodeRed)
 
  -stockage de la position (x,y,z) du divice

## 4) Creation de zone sur la carte (exemple salle B101,B102) ( gestion des couloirs a rajouter plutard )
 
  - suivi de la position du device dans les salle
 
  - stockage de la salle du device

## 5) en cliquant sur un device on peut avoir l'historique du device ( il est restée 10 min dans la piece x, puis 15 min dans la salle y, puis de nouveau 12 min dans la salle x )
    
## 6) etape facultative, pouvoir selectionner le divice et pouvoir suivre le chemin du divice


