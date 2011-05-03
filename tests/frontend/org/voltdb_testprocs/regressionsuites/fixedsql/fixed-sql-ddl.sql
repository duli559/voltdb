CREATE TABLE P1 (
  ID INTEGER DEFAULT '0' NOT NULL,
  DESC VARCHAR(300),
  NUM INTEGER,
  RATIO FLOAT,
  PRIMARY KEY (ID)
);

CREATE TABLE R1 (
  ID INTEGER DEFAULT '0' NOT NULL,
  DESC VARCHAR(300),
  NUM INTEGER,
  RATIO FLOAT,
  PRIMARY KEY (ID)
);

CREATE TABLE P2 (
  ID INTEGER DEFAULT '0' NOT NULL,
  DESC VARCHAR(300),
  NUM INTEGER NOT NULL,
  RATIO FLOAT NOT NULL,
  CONSTRAINT P2_PK_TREE PRIMARY KEY (ID)
);

CREATE TABLE R2 (
  ID INTEGER DEFAULT '0' NOT NULL,
  DESC VARCHAR(300),
  NUM INTEGER NOT NULL,
  RATIO FLOAT NOT NULL,
  CONSTRAINT R2_PK_TREE PRIMARY KEY (ID)
);

CREATE TABLE P1_DECIMAL (
  ID INTEGER DEFAULT '0' NOT NULL,
  CASH DECIMAL NOT NULL,
  CREDIT DECIMAL NOT NULL,
  RATIO FLOAT NOT NULL,
  PRIMARY KEY (ID)
);

CREATE TABLE R1_DECIMAL (
  ID INTEGER DEFAULT '0' NOT NULL,
  CASH DECIMAL NOT NULL,
  CREDIT DECIMAL NOT NULL,
  RATIO FLOAT NOT NULL,
  PRIMARY KEY (ID)
);

CREATE TABLE COUNT_NULL (
  TRICKY TINYINT,
  ID INTEGER DEFAULT '0' NOT NULL,
  NUM INTEGER DEFAULT '0' NOT NULL,
  PRIMARY KEY (ID)
);

CREATE TABLE OBJECT_DETAIL ( 
  OBJECT_DETAIL_ID INTEGER NOT NULL, 
  NAME VARCHAR(256) NOT NULL, 
  DESCRIPTION VARCHAR(1024) NOT NULL, 
  PRIMARY KEY (OBJECT_DETAIL_ID) 
); 

CREATE TABLE ASSET ( 
  ASSET_ID INTEGER NOT NULL, 
  OBJECT_DETAIL_ID INTEGER NOT NULL, 
  PRIMARY KEY (ASSET_ID) 
);

CREATE TABLE STRINGPART (
  NAME VARCHAR(9) NOT NULL,
  VAL1 INTEGER NOT NULL,
  VAL2 INTEGER,
  PRIMARY KEY(VAL1)
);

CREATE TABLE test_ENG1232 (
    id bigint NOT NULL,
    PRIMARY KEY (id)
);

