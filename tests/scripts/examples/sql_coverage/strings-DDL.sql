CREATE TABLE P1 (
  ID INTEGER DEFAULT '0' NOT NULL,
  DESC VARCHAR(5000),
  DESC_INLINE VARCHAR(15),
  DESC16 VARCHAR(16),
  DESC40B VARCHAR(40 BYTES),
  DESC63B VARCHAR(63 BYTES),
  DESC64B VARCHAR(64 BYTES),
  RATIO FLOAT NOT NULL,
  PRIMARY KEY (ID)
);

CREATE TABLE R1 (
  ID INTEGER DEFAULT '0' NOT NULL,
  DESC VARCHAR(5000),
  DESC_INLINE VARCHAR(15),
  DESC16 VARCHAR(16),
  DESC40B VARCHAR(40 BYTES),
  DESC63B VARCHAR(63 BYTES),
  DESC64B VARCHAR(64 BYTES),
  RATIO FLOAT NOT NULL,
  PRIMARY KEY (ID)
);
PARTITION TABLE P1 ON COLUMN ID;
